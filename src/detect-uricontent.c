/* Copyright (C) 2007-2010 Open Information Security Foundation
 *
 * You can copy, redistribute or modify this Program under the terms of
 * the GNU General Public License version 2 as published by the Free
 * Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * version 2 along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * \file
 *
 * \author  Victor Julien <victor@inliniac.net>
 * \author Gurvinder Singh <gurvindersinghdahiya@gmail.com>
 *
 * Simple uricontent match part of the detection engine.
 */

#include "suricata-common.h"
#include "decode.h"
#include "detect.h"
#include "detect-content.h"
#include "detect-uricontent.h"
#include "detect-engine-mpm.h"
#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-state.h"
#include "flow.h"
#include "detect-flow.h"
#include "flow-var.h"
#include "flow-util.h"
#include "threads.h"
#include "flow-alert-sid.h"

#include "stream-tcp.h"
#include "stream.h"
#include "app-layer-parser.h"
#include "app-layer-protos.h"
#include "app-layer-htp.h"

#include "util-mpm.h"
#include "util-print.h"
#include "util-debug.h"
#include "util-unittest.h"
#include "util-unittest-helper.h"
#include "util-binsearch.h"
#include "util-spm.h"
#include "util-spm-bm.h"

/* prototypes */
static int DetectUricontentSetup (DetectEngineCtx *, Signature *, char *);
void HttpUriRegisterTests(void);

int DetectAppLayerUricontentMatch (ThreadVars *, DetectEngineThreadCtx *,
                                   Flow *, uint8_t , void *,
                                   Signature *, SigMatch *);
void DetectUricontentFree(void *);

/**
 * \brief Registration function for uricontent: keyword
 */
void DetectUricontentRegister (void)
{
    sigmatch_table[DETECT_URICONTENT].name = "uricontent";
    sigmatch_table[DETECT_URICONTENT].AppLayerMatch = NULL;
    sigmatch_table[DETECT_URICONTENT].Match = NULL;
    sigmatch_table[DETECT_URICONTENT].Setup = DetectUricontentSetup;
    sigmatch_table[DETECT_URICONTENT].Free  = DetectUricontentFree;
    sigmatch_table[DETECT_URICONTENT].RegisterTests = HttpUriRegisterTests;
    sigmatch_table[DETECT_URICONTENT].alproto = ALPROTO_HTTP;

    sigmatch_table[DETECT_URICONTENT].flags |= SIGMATCH_PAYLOAD;
}

/**
 * \brief   pass on the uricontent_max_id
 * \param   de_ctx  pointer to the detect egine context whose max id is asked
 */
uint32_t DetectUricontentMaxId(DetectEngineCtx *de_ctx)
{
    return MpmPatternIdStoreGetMaxId(de_ctx->mpm_pattern_id_store);
}

/**
 * \brief this function will Free memory associated with DetectContentData
 *
 * \param cd pointer to DetectUricotentData
 */
void DetectUricontentFree(void *ptr) {
    SCEnter();
    DetectContentData *cd = (DetectContentData *)ptr;

    if (cd == NULL)
        SCReturn;

    if (cd->content != NULL)
        SCFree(cd->content);

    BoyerMooreCtxDeInit(cd->bm_ctx);

    SCFree(cd);
    SCReturn;
}

/**
 * \brief Helper function to print a DetectContentData
 */
void DetectUricontentPrint(DetectContentData *cd)
{
    int i = 0;
    if (cd == NULL) {
        SCLogDebug("Detect UricontentData \"cd\" is NULL");
        return;
    }
    char *tmpstr = SCMalloc(sizeof(char) * cd->content_len + 1);
    if (tmpstr == NULL)
        return;

    if (tmpstr != NULL) {
        for (i = 0; i < cd->content_len; i++) {
            if (isprint(cd->content[i]))
                tmpstr[i] = cd->content[i];
            else
                tmpstr[i] = '.';
        }
        tmpstr[i] = '\0';
        SCLogDebug("Uricontent: \"%s\"", tmpstr);
        SCFree(tmpstr);
    } else {
        SCLogDebug("Uricontent: ");
        for (i = 0; i < cd->content_len; i++)
            SCLogDebug("%c", cd->content[i]);
    }

    SCLogDebug("Uricontent_id: %"PRIu32, cd->id);
    SCLogDebug("Uricontent_len: %"PRIu16, cd->content_len);
    SCLogDebug("Depth: %"PRIu16, cd->depth);
    SCLogDebug("Offset: %"PRIu16, cd->offset);
    SCLogDebug("Within: %"PRIi32, cd->within);
    SCLogDebug("Distance: %"PRIi32, cd->distance);
    SCLogDebug("flags: %u ", cd->flags);
    SCLogDebug("negated: %s ",
            cd->flags & DETECT_CONTENT_NEGATED ? "true" : "false");
    SCLogDebug("relative match next: %s ",
            cd->flags & DETECT_CONTENT_RELATIVE_NEXT ? "true" : "false");
    SCLogDebug("-----------");
}


/**
 * \brief Search the first DETECT_URICONTENT
 * \retval pointer to the SigMatch holding the DetectUricontent
 * \param sm pointer to the current SigMatch of a parsing process
 * \retval null if no applicable DetectUricontent was found
 * \retval pointer to the SigMatch that has the previous SigMatch
 *                 of type DetectUricontent
 */
SigMatch *DetectUricontentGetLastPattern(SigMatch *sm)
{
    if (sm == NULL)
        return NULL;
    while (sm != NULL && sm->type != DETECT_URICONTENT)
        sm = sm->prev;

    if (sm == NULL)
        return NULL;

    DetectContentData *cd = (DetectContentData*) sm->ctx;
    if (cd == NULL)
        return NULL;

    return sm;
}

/**
 * \brief   Setup the detecturicontent keyword data from the string defined in
 *          the rule set.
 * \param   contentstr  Pointer to the string which has been defined in the rule
 */
DetectContentData *DoDetectUricontentSetup (char * contentstr)
{
    DetectContentData *cd = NULL;
    char *temp = NULL;
    char *str = NULL;
    uint16_t len = 0;
    uint16_t pos = 0;
    uint16_t slen = 0;

    if ((temp = SCStrdup(contentstr)) == NULL)
        goto error;

    if (strlen(temp) == 0) {
        SCFree(temp);
        return NULL;
    }

    cd = SCMalloc(sizeof(DetectContentData));
    if (cd == NULL)
        goto error;
    memset(cd,0,sizeof(DetectContentData));

    /* skip the first spaces */
    slen = strlen(temp);
    while (pos < slen && isspace(temp[pos])) {
        pos++;
    };

    if (temp[pos] == '!') {
        cd->flags |= DETECT_CONTENT_NEGATED;
        pos++;
    }

    if (temp[pos] == '\"' && strlen(temp + pos) == 1)
        goto error;

    if (temp[pos] == '\"' && temp[pos + strlen(temp + pos) - 1] == '\"') {
        if ((str = SCStrdup(temp + pos + 1)) == NULL)
            goto error;
        str[strlen(temp) - pos - 2] = '\0';
    } else {
        SCLogError(SC_ERR_INVALID_SIGNATURE, "uricontent keywords's argument "
                   "should be always enclosed in double quotes.  Invalid "
                   "content keyword passed in this rule - \"%s\"",
                   contentstr);
        goto error;
    }
    str[strlen(temp) - pos - 2] = '\0';

    SCFree(temp);
    temp = NULL;

    len = strlen(str);
    if (len == 0)
        goto error;

    SCLogDebug("\"%s\", len %" PRIu32 "", str, len);
    char converted = 0;

    {
        uint8_t escape = 0;
        uint16_t i, x;
        uint8_t bin = 0, binstr[3] = "", binpos = 0;
        uint16_t bin_count = 0;

        for (i = 0, x = 0; i < len; i++) {
            SCLogDebug("str[%02u]: %c", i, str[i]);
            if (str[i] == '|') {
                bin_count++;
                if (bin) {
                    bin = 0;
                } else {
                    bin = 1;
                }
            } else if(!escape && str[i] == '\\') {
                escape = 1;
            } else {
                if (bin) {
                    if (isdigit(str[i]) ||
                        str[i] == 'A' || str[i] == 'a' ||
                        str[i] == 'B' || str[i] == 'b' ||
                        str[i] == 'C' || str[i] == 'c' ||
                        str[i] == 'D' || str[i] == 'd' ||
                        str[i] == 'E' || str[i] == 'e' ||
                        str[i] == 'F' || str[i] == 'f') {
                        SCLogDebug("part of binary: %c", str[i]);

                        binstr[binpos] = (char)str[i];
                        binpos++;

                        if (binpos == 2) {
                            uint8_t c = strtol((char *)binstr, (char **) NULL,
                                                16) & 0xFF;
                            binpos = 0;
                            str[x] = c;
                            x++;
                            converted = 1;
                        }
                    } else if (str[i] == ' ') {
                        SCLogDebug("space as part of binary string");
                    }
                } else if (escape) {
                    if (str[i] == ':' ||
                        str[i] == ';' ||
                        str[i] == '\\' ||
                        str[i] == '\"')
                    {
                        str[x] = str[i];
                        x++;
                    } else {
                        //SCLogDebug("Can't escape %c", str[i]);
                        goto error;
                    }
                    escape = 0;
                    converted = 1;
                } else {
                    str[x] = str[i];
                    x++;
                }
            }
        }

        if (bin_count % 2 != 0) {
            SCLogError(SC_ERR_INVALID_SIGNATURE, "Invalid hex code assembly in "
                       "content - %s.  Invalidating signature", str);
            goto error;
        }

#ifdef DEBUG
        if (SCLogDebugEnabled()) {
            for (i = 0; i < x; i++) {
                if (isprint(str[i])) printf("%c", str[i]);
                else                 printf("\\x%02u", str[i]);
            }
            printf("\n");
        }
#endif

        if (converted)
            len = x;
    }

    SCLogDebug("len %" PRIu32 "", len);

    cd->content = SCMalloc(len);
    if (cd->content == NULL) {
        SCFree(cd);
        SCFree(str);
        return NULL;;
    }

    memcpy(cd->content, str, len);
    cd->content_len = len;
    cd->depth = 0;
    cd->offset = 0;
    cd->within = 0;
    cd->distance = 0;

    /* Prepare Boyer Moore context for searching faster */
    cd->bm_ctx = BoyerMooreCtxInit(cd->content, cd->content_len);

    SCFree(str);
    return cd;

error:
    SCFree(str);
    if (cd) SCFree(cd);
    return NULL;
}

/**
 * \brief Creates a SigMatch for the uricontent keyword being sent as argument,
 *        and appends it to the Signature(s).
 *
 * \param de_ctx    Pointer to the detection engine context
 * \param s         Pointer to signature for the current Signature being parsed
 *                  from the rules
 * \param contentstr  Pointer to the string holding the keyword value
 *
 * \retval 0 on success, -1 on failure
 */
int DetectUricontentSetup (DetectEngineCtx *de_ctx, Signature *s, char *contentstr)
{
    SCEnter();

    DetectContentData *cd = NULL;
    SigMatch *sm = NULL;

    if (s->alproto == ALPROTO_DCERPC) {
        SCLogError(SC_ERR_INVALID_SIGNATURE, "uri content specified in a dcerpc sig");
        goto error;
    }

    cd = DoDetectUricontentSetup(contentstr);
    if (cd == NULL)
        goto error;

    /* Okay so far so good, lets get this into a SigMatch
     * and put it in the Signature. */
    sm = SigMatchAlloc();
    if (sm == NULL)
        goto error;

    sm->type = DETECT_URICONTENT;
    sm->ctx = (void *)cd;

    cd->id = DetectUricontentGetId(de_ctx->mpm_pattern_id_store, cd);

    /* Flagged the signature as to inspect the app layer data */
    s->flags |= SIG_FLAG_APPLAYER;

    if (s->alproto != ALPROTO_UNKNOWN && s->alproto != ALPROTO_HTTP) {
        SCLogError(SC_ERR_CONFLICTING_RULE_KEYWORDS, "rule contains conflicting"
                " keywords.");
        goto error;
    }

    s->alproto = ALPROTO_HTTP;

    SigMatchAppendUricontent(s,sm);

    SCReturnInt(0);

error:
    if (cd) SCFree(cd);
    if (sm != NULL) SCFree(sm);
    SCReturnInt(-1);
}

/**
 * \brief   Checks if the content sent as the argument, has a uricontent which
 *          has been provided in the rule. This match function matches the
 *          normalized http uri against the given rule using multi pattern
 *          search algorithms.
 *
 * \param det_ctx       Pointer to the detection engine thread context
 * \param content       Pointer to the uri content currently being matched
 * \param content_len   Content_len of the received uri content
 *
 * \retval 1 if the uri contents match; 0 no match
 */
static inline int DoDetectAppLayerUricontentMatch (DetectEngineThreadCtx *det_ctx,
                                     uint8_t *uri, uint16_t uri_len)
{
    int ret = 0;
    /* run the pattern matcher against the uri */
    if (det_ctx->sgh->mpm_uricontent_maxlen > uri_len) {
        SCLogDebug("not searching as pkt payload is smaller than the "
                "largest uricontent length we need to match");
    } else {
        SCLogDebug("search: (%p, maxlen %" PRIu32 ", sgh->sig_cnt "
                "%" PRIu32 ")", det_ctx->sgh, det_ctx->sgh->
                mpm_uricontent_maxlen, det_ctx->sgh->sig_cnt);

        det_ctx->uris++;

        if (det_ctx->sgh->mpm_uricontent_maxlen == 1) det_ctx->pkts_uri_searched1++;
        else if (det_ctx->sgh->mpm_uricontent_maxlen == 2) det_ctx->pkts_uri_searched2++;
        else if (det_ctx->sgh->mpm_uricontent_maxlen == 3) det_ctx->pkts_uri_searched3++;
        else if (det_ctx->sgh->mpm_uricontent_maxlen == 4) det_ctx->pkts_uri_searched4++;
        else det_ctx->pkts_uri_searched++;

        ret += UriPatternSearch(det_ctx, uri, uri_len);

        SCLogDebug("post search: cnt %" PRIu32, ret);
    }
    return ret;
}

/**
 *  \brief Run the pattern matcher against the uri(s)
 *
 *  We run against _all_ uri(s) we have as the pattern matcher will
 *  flag each sig that has a match. We need to do this for all uri(s)
 *  to not miss possible events.
 *
 *  \param f locked flow
 *  \param htp_state initialized htp state
 *
 *  \warning Make sure the flow/state is locked
 *  \todo what should we return? Just the fact that we matched?
 */
uint32_t DetectUricontentInspectMpm(DetectEngineThreadCtx *det_ctx, Flow *f, HtpState *htp_state) {
    SCEnter();

    uint32_t cnt = 0;
    int idx = 0;
    htp_tx_t *tx = NULL;

    /* locking the flow, we will inspect the htp state */
#ifdef __tile__
    tmc_spin_queued_mutex_lock(&f->m);
#else
    SCMutexLock(&f->m);
#endif

    if (htp_state == NULL || htp_state->connp == NULL) {
        SCLogDebug("no HTTP state / no connp");
#ifdef __tile__
        tmc_spin_queued_mutex_unlock(&f->m);
#else
        SCMutexUnlock(&f->m);
#endif
        SCReturnUInt(0U);
    }

    idx = AppLayerTransactionGetInspectId(f);
    if (idx == -1) {
        goto end;
    }

    int size = (int)list_size(htp_state->connp->conn->transactions);
    for (; idx < size; idx++)
    {
        tx = list_get(htp_state->connp->conn->transactions, idx);
        if (tx == NULL || tx->request_uri_normalized == NULL)
            continue;

        cnt += DoDetectAppLayerUricontentMatch(det_ctx, (uint8_t *)
                bstr_ptr(tx->request_uri_normalized),
                bstr_len(tx->request_uri_normalized));
    }
end:
#ifdef __tile__
    tmc_spin_queued_mutex_unlock(&f->m);
#else
    SCMutexUnlock(&f->m);
#endif
    SCReturnUInt(cnt);
}

/*
 * UNITTTESTS
 */

#ifdef UNITTESTS

#include "stream-tcp-reassemble.h"

/** \test Test case where path traversal has been sent as a path string in the
 *        HTTP URL and normalized path string is checked */
static int HTTPUriTest01(void) {
    int result = 0;
    Flow f;
    uint8_t httpbuf1[] = "GET /../../images.gif HTTP/1.1\r\nHost: www.ExA"
                         "mPlE.cOM\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    int r = 0;
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.flags |= FLOW_IPV4;

    StreamTcpInitConfig(TRUE);

    r = AppLayerParse(NULL, &f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_START|
                          STREAM_EOF, httpbuf1, httplen1);
    if (r != 0) {
        printf("AppLayerParse failed: r(%d) != 0: ", r);
        goto end;
    }

    HtpState *htp_state = f.alstate;
    if (htp_state == NULL) {
        printf("no http state: ");
        goto end;
    }

    htp_tx_t *tx = list_get(htp_state->connp->conn->transactions, 0);

    if (htp_state->connp == NULL || tx->request_method_number != M_GET ||
            tx->request_protocol_number != HTTP_1_1)
    {
        printf("expected method GET and got %s: , expected protocol "
                "HTTP/1.1 and got %s \n", bstr_tocstr(tx->request_method),
                bstr_tocstr(tx->request_protocol));
        goto end;
    }

    if ((tx->parsed_uri->hostname == NULL) ||
            (bstr_cmpc(tx->parsed_uri->hostname, "www.example.com") != 0))
    {
        printf("expected www.example.com as hostname, but got: %s \n",
                bstr_tocstr(tx->parsed_uri->hostname));
        goto end;
    }

    if ((tx->parsed_uri->path == NULL) ||
            (bstr_cmpc(tx->parsed_uri->path, "/images.gif") != 0))
    {
        printf("expected /images.gif as path, but got: %s \n",
                bstr_tocstr(tx->parsed_uri->path));
        goto end;
    }

    result = 1;
end:
    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    return result;
}

/** \test Test case where path traversal has been sent in special characters in
 *        HEX encoding in the HTTP URL and normalized path string is checked */
static int HTTPUriTest02(void) {
    int result = 0;
    Flow f;
    HtpState *htp_state = NULL;
    uint8_t httpbuf1[] = "GET /%2e%2e/images.gif HTTP/1.1\r\nHost: www.ExA"
                         "mPlE.cOM\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    int r = 0;
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.flags |= FLOW_IPV4;

    StreamTcpInitConfig(TRUE);

    r = AppLayerParse(NULL, &f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_START|
                          STREAM_EOF, httpbuf1, httplen1);
    if (r != 0) {
        printf("AppLayerParse failed: r(%d) != 0: ", r);
        goto end;
    }

    htp_state = f.alstate;
    if (htp_state == NULL) {
        printf("no http state: ");
        goto end;
    }

    htp_tx_t *tx = list_get(htp_state->connp->conn->transactions, 0);

    if (htp_state->connp == NULL || tx->request_method_number != M_GET ||
            tx->request_protocol_number != HTTP_1_1)
    {
        printf("expected method GET and got %s: , expected protocol "
                "HTTP/1.1 and got %s \n", bstr_tocstr(tx->request_method),
                bstr_tocstr(tx->request_protocol));
        goto end;
    }

    if ((tx->parsed_uri->hostname == NULL) ||
            (bstr_cmpc(tx->parsed_uri->hostname, "www.example.com") != 0))
    {
        printf("expected www.example.com as hostname, but got: %s \n",
                bstr_tocstr(tx->parsed_uri->hostname));
        goto end;
    }

    if ((tx->parsed_uri->path == NULL) ||
            (bstr_cmpc(tx->parsed_uri->path, "/images.gif") != 0))
    {
        printf("expected /images.gif as path, but got: %s \n",
                bstr_tocstr(tx->parsed_uri->path));
        goto end;
    }

    result = 1;
end:
    StreamTcpFreeConfig(TRUE);
    if (htp_state != NULL)
        HTPStateFree(htp_state);
    FLOW_DESTROY(&f);
    return result;
}

/** \test Test case where NULL character has been sent in HEX encoding in the
 *        HTTP URL and normalized path string is checked */
static int HTTPUriTest03(void) {
    int result = 0;
    Flow f;
    HtpState *htp_state = NULL;
    uint8_t httpbuf1[] = "GET%00 /images.gif HTTP/1.1\r\nHost: www.ExA"
                         "mPlE.cOM\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    int r = 0;
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.flags |= FLOW_IPV4;

    StreamTcpInitConfig(TRUE);

    r = AppLayerParse(NULL, &f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_START|
                          STREAM_EOF, httpbuf1, httplen1);
    if (r != 0) {
        printf("AppLayerParse failed: r(%d) != 0: ", r);
        goto end;
    }

    htp_state = f.alstate;
    if (htp_state == NULL) {
        printf("no http state: ");
        goto end;
    }

    htp_tx_t *tx = list_get(htp_state->connp->conn->transactions, 0);

    if (htp_state->connp == NULL || tx->request_method_number != M_UNKNOWN ||
            tx->request_protocol_number != HTTP_1_1)
    {
        printf("expected method GET and got %s: , expected protocol "
                "HTTP/1.1 and got %s \n", bstr_tocstr(tx->request_method),
                bstr_tocstr(tx->request_protocol));
        goto end;
    }

   if ((tx->parsed_uri->hostname == NULL) ||
            (bstr_cmpc(tx->parsed_uri->hostname, "www.example.com") != 0))
    {
        printf("expected www.example.com as hostname, but got: %s \n",
                bstr_tocstr(tx->parsed_uri->hostname));
        goto end;
    }

    if ((tx->parsed_uri->path == NULL) ||
            (bstr_cmpc(tx->parsed_uri->path, "/images.gif") != 0))
    {
        printf("expected /images.gif as path, but got: %s \n",
                bstr_tocstr(tx->parsed_uri->path));
        goto end;
    }

    result = 1;
end:
    StreamTcpFreeConfig(TRUE);
    if (htp_state != NULL)
        HTPStateFree(htp_state);
    FLOW_DESTROY(&f);
    return result;
}


/** \test Test case where self referencing directories request has been sent
 *        in the HTTP URL and normalized path string is checked */
static int HTTPUriTest04(void) {
    int result = 0;
    Flow f;
    HtpState *htp_state = NULL;
    uint8_t httpbuf1[] = "GET /./././images.gif HTTP/1.1\r\nHost: www.ExA"
                         "mPlE.cOM\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    int r = 0;
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.flags |= FLOW_IPV4;

    StreamTcpInitConfig(TRUE);

    r = AppLayerParse(NULL, &f, ALPROTO_HTTP, STREAM_TOSERVER|STREAM_START|
                          STREAM_EOF, httpbuf1, httplen1);
    if (r != 0) {
        printf("AppLayerParse failed: r(%d) != 0: ", r);
        goto end;
    }

    htp_state = f.alstate;
    if (htp_state == NULL) {
        printf("no http state: ");
        result = 0;
        goto end;
    }

    htp_tx_t *tx = list_get(htp_state->connp->conn->transactions, 0);

    if (htp_state->connp == NULL || tx->request_method_number != M_GET ||
            tx->request_protocol_number != HTTP_1_1)
    {
        printf("expected method GET and got %s: , expected protocol "
                "HTTP/1.1 and got %s \n", bstr_tocstr(tx->request_method),
                bstr_tocstr(tx->request_protocol));
        result = 0;
        goto end;
    }

    if ((tx->parsed_uri->hostname == NULL) ||
            (bstr_cmpc(tx->parsed_uri->hostname, "www.example.com") != 0))
    {
        printf("expected www.example.com as hostname, but got: %s \n",
                bstr_tocstr(tx->parsed_uri->hostname));
        result = 0;
        goto end;
    }

    if ((tx->parsed_uri->path == NULL) ||
           (bstr_cmpc(tx->parsed_uri->path, "/images.gif") != 0))
    {
        printf("expected /images.gif as path, but got: %s \n",
                bstr_tocstr(tx->parsed_uri->path));
        result = 0;
        goto end;
    }

    result = 1;
end:
    StreamTcpFreeConfig(TRUE);
    if (htp_state != NULL)
        HTPStateFree(htp_state);
    FLOW_DESTROY(&f);
    return result;
}

/**
 * \test Checks if a uricontent is registered in a Signature
 */
int DetectUriSigTest01(void)
{
    SigMatch *sm = NULL;
    int result = 0;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    Signature *s = NULL;

    memset(&th_v, 0, sizeof(th_v));

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }
    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert http any any -> any any (msg:"
                                   "\" Test uricontent\"; "
                                  "uricontent:\"me\"; sid:1;)");
    if (s == NULL) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    BUG_ON(de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH] == NULL);

    sm = de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH];
    if (sm->type == DETECT_URICONTENT) {
        result = 1;
    } else {
        result = 0;
    }

 end:
    if (de_ctx != NULL) SigGroupCleanup(de_ctx);
    if (de_ctx != NULL) SigCleanSignatures(de_ctx);
    if (det_ctx != NULL) DetectEngineThreadCtxDeinit(&th_v, det_ctx);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);
    return result;
}

/** \test Check the signature working to alert when http_cookie is matched . */
static int DetectUriSigTest02(void) {
    int result = 0;
    Flow f;
    uint8_t httpbuf1[] = "POST /one HTTP/1.0\r\nUser-Agent: Mozilla/1.0\r\nCookie:"
                         " hellocatch\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet *p = NULL;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    HtpState *http_state = NULL;

    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(httpbuf1, httplen1, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }
    de_ctx->mpm_matcher = MPM_B2G;
    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent\"; "
                                   "uricontent:\"foo\"; sid:1;)");
    if (s == NULL) {
        goto end;
    }

    s = s->next = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent\"; "
                                   "uricontent:\"one\"; sid:2;)");
    if (s == NULL) {
        goto end;
    }

    s = s->next = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent\"; "
                                   "uricontent:\"oisf\"; sid:3;)");
    if (s == NULL) {
        goto end;
    }


    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    int r = AppLayerParse(NULL, &f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        goto end;
    }

    http_state = f.alstate;
    if (http_state == NULL) {
        printf("no http state: ");
        goto end;
    }

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    if ((PacketAlertCheck(p, 1))) {
        printf("sig: 1 alerted, but it should not\n");
        goto end;
    } else if (!PacketAlertCheck(p, 2)) {
        printf("sig: 2 did not alerted, but it should\n");
        goto end;
    }  else if ((PacketAlertCheck(p, 3))) {
        printf("sig: 3 alerted, but it should not\n");
        goto end;
    }

    result = 1;
end:
    //if (http_state != NULL) HTPStateFree(http_state);
    if (de_ctx != NULL) SigCleanSignatures(de_ctx);
    if (de_ctx != NULL) SigGroupCleanup(de_ctx);
    if (det_ctx != NULL) DetectEngineThreadCtxDeinit(&th_v, det_ctx);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);

    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    UTHFreePackets(&p, 1);
    return result;
}

/** \test Check the working of search once per packet only in applayer
 *        match */
static int DetectUriSigTest03(void) {
    int result = 0;
    Flow f;
    HtpState *http_state = NULL;
    uint8_t httpbuf1[] = "POST /one HTTP/1.0\r\nUser-Agent: Mozilla/1.0\r\nCookie:"
                         " hellocatch\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    uint8_t httpbuf2[] = "POST /oneself HTTP/1.0\r\nUser-Agent: Mozilla/1.0\r\nCookie:"
                         " hellocatch\r\n\r\n";
    uint32_t httplen2 = sizeof(httpbuf2) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet *p = NULL;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;

    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(httpbuf1, httplen1, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }
    de_ctx->mpm_matcher = MPM_B2G;
    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent\"; "
                                   "uricontent:\"foo\"; sid:1;)");
    if (s == NULL) {
        goto end;
    }

   s = s->next = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent\"; "
                                   "uricontent:\"one\"; sid:2;)");
    if (s == NULL) {
        goto end;
    }

    s = s->next = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent\"; "
                                   "uricontent:\"self\"; sid:3;)");
    if (s == NULL) {
        goto end;
    }


    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    int r = AppLayerParse(NULL, &f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        goto end;
    }

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    if ((PacketAlertCheck(p, 1))) {
        printf("sig 1 alerted, but it should not: ");
        goto end;
    } else if (!PacketAlertCheck(p, 2)) {
        printf("sig 2 did not alert, but it should: ");
        goto end;
    } else if ((PacketAlertCheck(p, 3))) {
        printf("sig 3 alerted, but it should not: ");
        goto end;
    }


    r = AppLayerParse(NULL, &f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf2, httplen2);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        goto end;
    }

    http_state = f.alstate;
    if (http_state == NULL) {
        printf("no http state: ");
        goto end;
    }

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    if ((PacketAlertCheck(p, 1))) {
        printf("sig 1 alerted, but it should not (chunk 2): ");
        goto end;
    } else if (PacketAlertCheck(p, 2)) {
        printf("sig 2 alerted, but it should not (chunk 2): ");
        goto end;
    } else if (!(PacketAlertCheck(p, 3))) {
        printf("sig 3 did not alert, but it should (chunk 2): ");
        goto end;
    }

    result = 1;

end:
    if (de_ctx != NULL) SigGroupCleanup(de_ctx);
    if (de_ctx != NULL) SigCleanSignatures(de_ctx);
    if (det_ctx != NULL) DetectEngineThreadCtxDeinit(&th_v, det_ctx);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);

    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    UTHFreePackets(&p, 1);
    return result;
}

/**
 * \test Check that modifiers of content apply only to content keywords
 *       and the same for uricontent modifiers
 */
static int DetectUriSigTest04(void) {
    int result = 0;
    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    Signature *s = NULL;

    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent\"; "
                                   "uricontent:\"foo\"; sid:1;)");
    if (s == NULL ||
        s->sm_lists[DETECT_SM_LIST_UMATCH] == NULL ||
        s->sm_lists[DETECT_SM_LIST_PMATCH] != NULL ||
        s->sm_lists[DETECT_SM_LIST_MATCH] != NULL)
    {
        printf("sig 1 failed to parse: ");
        goto end;
    }

    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent and content\"; "
                                   "uricontent:\"foo\"; content:\"bar\";sid:1;)");
    if (s == NULL ||
        s->sm_lists[DETECT_SM_LIST_UMATCH] == NULL ||
        s->sm_lists[DETECT_SM_LIST_PMATCH] == NULL ||
        s->sm_lists[DETECT_SM_LIST_MATCH] != NULL)
    {
        printf("sig 2 failed to parse: ");
        goto end;
    }

    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent and content\"; "
                                   "uricontent:\"foo\"; content:\"bar\";"
                                   " depth:10; offset: 5; sid:1;)");
    if (s == NULL ||
        s->sm_lists[DETECT_SM_LIST_UMATCH] == NULL ||
        s->sm_lists[DETECT_SM_LIST_PMATCH] == NULL ||
        ((DetectContentData *)s->sm_lists[DETECT_SM_LIST_PMATCH]->ctx)->depth != 15 ||
        ((DetectContentData *)s->sm_lists[DETECT_SM_LIST_PMATCH]->ctx)->offset != 5 ||
        s->sm_lists[DETECT_SM_LIST_MATCH] != NULL)
    {
        printf("sig 3 failed to parse: ");
        goto end;
    }

    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent and content\"; "
                                   "content:\"foo\"; uricontent:\"bar\";"
                                   " depth:10; offset: 5; sid:1;)");
    if (s == NULL ||
        s->sm_lists[DETECT_SM_LIST_UMATCH] == NULL ||
        s->sm_lists[DETECT_SM_LIST_PMATCH] == NULL ||
        ((DetectContentData *)s->sm_lists[DETECT_SM_LIST_UMATCH]->ctx)->depth != 15 ||
        ((DetectContentData *)s->sm_lists[DETECT_SM_LIST_UMATCH]->ctx)->offset != 5 ||
        s->sm_lists[DETECT_SM_LIST_MATCH] != NULL)
    {
        printf("sig 4 failed to parse: ");
        goto end;
    }

    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent and content\"; "
                                   "uricontent:\"foo\"; content:\"bar\";"
                                   " depth:10; offset: 5; within:3; sid:1;)");
    if (s != NULL) {
        printf("sig 5 failed to parse: ");
        goto end;
    }

    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent and content\"; "
                                   "uricontent:\"foo\"; content:\"bar\";"
                                   " depth:10; offset: 5; distance:3; sid:1;)");
    if (s != NULL) {
        printf("sig 6 failed to parse: ");
        goto end;
    }

    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent and content\"; "
                                   "uricontent:\"foo\"; content:\"bar\";"
                                   " depth:10; offset: 5; content:"
                                   "\"two_contents\"; within:30; sid:1;)");
    if (s == NULL) {
        goto end;
    } else if (s->sm_lists[DETECT_SM_LIST_UMATCH] == NULL ||
            s->sm_lists[DETECT_SM_LIST_PMATCH] == NULL ||
            ((DetectContentData*) s->sm_lists[DETECT_SM_LIST_PMATCH]->ctx)->depth != 15 ||
            ((DetectContentData*) s->sm_lists[DETECT_SM_LIST_PMATCH]->ctx)->offset != 5 ||
            ((DetectContentData*) s->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx)->within != 30 ||
            s->sm_lists[DETECT_SM_LIST_MATCH] != NULL)
    {
        printf("sig 7 failed to parse: ");
        DetectContentPrint((DetectContentData*) s->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx);
        goto end;
    }

    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent and content\"; "
                                   "uricontent:\"foo\"; content:\"bar\";"
                                   " depth:10; offset: 5; uricontent:"
                                   "\"two_uricontents\"; within:30; sid:1;)");
    if (s == NULL) {
        goto end;
    } else if (s->sm_lists[DETECT_SM_LIST_UMATCH] == NULL ||
            s->sm_lists[DETECT_SM_LIST_PMATCH] == NULL ||
            ((DetectContentData*) s->sm_lists[DETECT_SM_LIST_PMATCH]->ctx)->depth != 15 ||
            ((DetectContentData*) s->sm_lists[DETECT_SM_LIST_PMATCH]->ctx)->offset != 5 ||
            ((DetectContentData*) s->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx)->within != 30 ||
            s->sm_lists[DETECT_SM_LIST_MATCH] != NULL)
    {
        printf("sig 8 failed to parse: ");
        DetectUricontentPrint((DetectContentData*) s->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx);
        goto end;
    }

    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent and content\"; "
                                   "uricontent:\"foo\"; content:\"bar\";"
                                   " depth:10; offset: 5; content:"
                                   "\"two_contents\"; distance:30; sid:1;)");
    if (s == NULL) {
        goto end;
    } else if (
            s->sm_lists[DETECT_SM_LIST_UMATCH] == NULL ||
            s->sm_lists[DETECT_SM_LIST_PMATCH] == NULL ||
            ((DetectContentData*) s->sm_lists[DETECT_SM_LIST_PMATCH]->ctx)->depth != 15 ||
            ((DetectContentData*) s->sm_lists[DETECT_SM_LIST_PMATCH]->ctx)->offset != 5 ||
            ((DetectContentData*) s->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx)->distance != 30 ||
            s->sm_lists[DETECT_SM_LIST_MATCH] != NULL)
    {
        printf("sig 9 failed to parse: ");
        DetectContentPrint((DetectContentData*) s->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx);
        goto end;
    }

    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent and content\"; "
                                   "uricontent:\"foo\"; content:\"bar\";"
                                   " depth:10; offset: 5; uricontent:"
                                   "\"two_uricontents\"; distance:30; sid:1;)");
    if (s == NULL) {
        goto end;
    } else if (
            s->sm_lists[DETECT_SM_LIST_UMATCH] == NULL ||
            s->sm_lists[DETECT_SM_LIST_PMATCH] == NULL ||
            ((DetectContentData*) s->sm_lists[DETECT_SM_LIST_PMATCH]->ctx)->depth != 15 ||
            ((DetectContentData*) s->sm_lists[DETECT_SM_LIST_PMATCH]->ctx)->offset != 5 ||
            ((DetectContentData*) s->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx)->distance != 30 ||
            s->sm_lists[DETECT_SM_LIST_MATCH] != NULL)
    {
        printf("sig 10 failed to parse: ");
        DetectUricontentPrint((DetectContentData*) s->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx);
        goto end;
    }

    s = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent and content\"; "
                                   "uricontent:\"foo\"; content:\"bar\";"
                                   " depth:10; offset: 5; uricontent:"
                                   "\"two_uricontents\"; distance:30; "
                                   "within:60; content:\"two_contents\";"
                                   " within:70; distance:45; sid:1;)");
    if (s == NULL) {
        printf("sig 10 failed to parse: ");
        goto end;
    }

    if (s->sm_lists[DETECT_SM_LIST_UMATCH] == NULL || s->sm_lists[DETECT_SM_LIST_PMATCH] == NULL) {
        printf("umatch %p or pmatch %p: ", s->sm_lists[DETECT_SM_LIST_UMATCH], s->sm_lists[DETECT_SM_LIST_PMATCH]);
        goto end;
    }

    if (    ((DetectContentData*) s->sm_lists[DETECT_SM_LIST_PMATCH]->ctx)->depth != 15 ||
            ((DetectContentData*) s->sm_lists[DETECT_SM_LIST_PMATCH]->ctx)->offset != 5 ||
            ((DetectContentData*) s->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx)->distance != 30 ||
            ((DetectContentData*) s->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx)->within != 60 ||
            ((DetectContentData*) s->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx)->distance != 45 ||
            ((DetectContentData*) s->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx)->within != 70 ||
            s->sm_lists[DETECT_SM_LIST_MATCH] != NULL) {
        printf("sig 10 failed to parse, content not setup properly: ");
        DetectContentPrint((DetectContentData*) s->sm_lists[DETECT_SM_LIST_PMATCH]->ctx);
        DetectUricontentPrint((DetectContentData*) s->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx);
        DetectContentPrint((DetectContentData*) s->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx);
        goto end;
    }

    result = 1;
end:
    if (de_ctx != NULL) SigCleanSignatures(de_ctx);
    if (de_ctx != NULL) SigGroupCleanup(de_ctx);
    return result;
}

/** \test Check the modifiers for uricontent and content
 *        match
 */
static int DetectUriSigTest05(void) {
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Flow f;
    HtpState *http_state = NULL;
    uint8_t httpbuf1[] = "POST /one/two/three HTTP/1.0\r\nUser-Agent: Mozilla/1.0\r\nCookie:"
                         " hellocatch\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet *p = NULL;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;

    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));


    p = UTHBuildPacket(httpbuf1, httplen1, IPPROTO_TCP);
    p->tcph->th_seq = htonl(1000);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;
    f.proto = p->proto;

    StreamTcpInitConfig(TRUE);

    StreamMsg *stream_msg = StreamMsgGetFromPool();
    if (stream_msg == NULL) {
        printf("no stream_msg: ");
        goto end;
    }

    memcpy(stream_msg->data.data, httpbuf1, httplen1);
    stream_msg->data.data_len = httplen1;

    ssn.toserver_smsg_head = stream_msg;
    ssn.toserver_smsg_tail = stream_msg;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }
    de_ctx->mpm_matcher = MPM_B2G;
    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
            "\" Test uricontent\"; uricontent:\"foo\"; sid:1;)");
    if (s == NULL) {
        goto end;
    }

    s = s->next = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
            "\" Test uricontent\"; uricontent:\"one\"; content:\"two\"; sid:2;)");
    if (s == NULL) {
        goto end;
    }

    s = s->next = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
            "\" Test uricontent\"; uricontent:\"one\"; offset:1; depth:10; "
            "uricontent:\"two\"; distance:1; within: 4; uricontent:\"three\"; "
            "distance:1; within: 6; sid:3;)");
    if (s == NULL) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    int r = AppLayerParse(NULL, &f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        goto end;
    }

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    http_state = f.alstate;
    if (http_state == NULL) {
        printf("no http state: ");
        goto end;
    }

    if ((PacketAlertCheck(p, 1))) {
        printf("sig: 1 alerted, but it should not: ");
        goto end;
    } else if (! PacketAlertCheck(p, 2)) {
        printf("sig: 2 did not alert, but it should: ");
        goto end;
    } else if (! (PacketAlertCheck(p, 3))) {
        printf("sig: 3 did not alert, but it should: ");
        goto end;
    }

    result = 1;
end:
    if (de_ctx != NULL) SigGroupCleanup(de_ctx);
    if (de_ctx != NULL) SigCleanSignatures(de_ctx);
    if (det_ctx != NULL) DetectEngineThreadCtxDeinit(&th_v, det_ctx);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);

    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    UTHFreePackets(&p, 1);
    return result;
}

/** \test Check the modifiers for uricontent and content
 *        match
 */
static int DetectUriSigTest06(void) {
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;
    Flow f;
    HtpState *http_state = NULL;
    uint8_t httpbuf1[] = "POST /one/two/three HTTP/1.0\r\nUser-Agent: Mozilla/1.0\r\nCookie:"
                         " hellocatch\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet *p = NULL;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;
    TCPHdr tcp_hdr;

    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));
    memset(&tcp_hdr, 0, sizeof(tcp_hdr));


    p = UTHBuildPacket(httpbuf1, httplen1, IPPROTO_TCP);
    p->tcph->th_seq = htonl(1000);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;
    f.proto = p->proto;

    StreamTcpInitConfig(TRUE);

    StreamMsg *stream_msg = StreamMsgGetFromPool();
    if (stream_msg == NULL) {
        printf("no stream_msg: ");
        goto end;
    }

    memcpy(stream_msg->data.data, httpbuf1, httplen1);
    stream_msg->data.data_len = httplen1;

    ssn.toserver_smsg_head = stream_msg;
    ssn.toserver_smsg_tail = stream_msg;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }
    de_ctx->mpm_matcher = MPM_B2G;
    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent\"; "
                                   "uricontent:\"foo\"; content:\"bar\"; sid:1;)");
    if (s == NULL) {
        goto end;
    }

    s = s->next = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent\"; "
                                   "uricontent:\"one\"; offset:1; depth:10; "
                                   "content:\"one\"; offset:1; depth:10; "
                                   "uricontent:\"two\"; distance:1; within: 4; "
                                   "content:\"two\"; distance:1; within: 4; "
                                   "uricontent:\"three\"; distance:1; within: 6; "
                                   "content:\"/three\"; distance:0; within: 7; "
                                   "sid:2;)");

    if (s == NULL) {
        goto end;
    }

    s = s->next = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent\"; "
                                   "uricontent:\"one\"; offset:1; depth:10; "
                                   "uricontent:\"two\"; distance:1; within: 4; "
                                   "uricontent:\"three\"; distance:1; within: 6; "
                                   "sid:3;)");

    if (s == NULL) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    int r = AppLayerParse(NULL, &f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        goto end;
    }

   /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    http_state = f.alstate;
    if (http_state == NULL) {
        printf("no http state: ");
        goto end;
    }

    if ((PacketAlertCheck(p, 1))) {
        printf("sig: 1 alerted, but it should not:");
        goto end;
    } else if (! PacketAlertCheck(p, 2)) {
        printf("sig: 2 did not alert, but it should:");
        goto end;
    } else if (! (PacketAlertCheck(p, 3))) {
        printf("sig: 3 did not alert, but it should:");
        goto end;
    }

    result = 1;
end:

    if (de_ctx != NULL) SigGroupCleanup(de_ctx);
    if (de_ctx != NULL) SigCleanSignatures(de_ctx);
    if (det_ctx != NULL) DetectEngineThreadCtxDeinit(&th_v, det_ctx);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);

    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    UTHFreePackets(&p, 1);
    return result;
}

/** \test Check the modifiers for uricontent and content
 *        match
 */
static int DetectUriSigTest07(void) {
    int result = 0;
    Flow f;
    HtpState *http_state = NULL;
    uint8_t httpbuf1[] = "POST /one/two/three HTTP/1.0\r\nUser-Agent: Mozilla/1.0\r\nCookie:"
                         " hellocatch\r\n\r\n";
    uint32_t httplen1 = sizeof(httpbuf1) - 1; /* minus the \0 */
    TcpSession ssn;
    Packet *p = NULL;
    Signature *s = NULL;
    ThreadVars th_v;
    DetectEngineThreadCtx *det_ctx = NULL;

    memset(&th_v, 0, sizeof(th_v));
    memset(&f, 0, sizeof(f));
    memset(&ssn, 0, sizeof(ssn));

    p = UTHBuildPacket(httpbuf1, httplen1, IPPROTO_TCP);

    FLOW_INITIALIZE(&f);
    f.protoctx = (void *)&ssn;
    f.flags |= FLOW_IPV4;

    p->flow = &f;
    p->flowflags |= FLOW_PKT_TOSERVER;
    p->flowflags |= FLOW_PKT_ESTABLISHED;
    p->flags |= PKT_HAS_FLOW|PKT_STREAM_EST;
    f.alproto = ALPROTO_HTTP;

    StreamTcpInitConfig(TRUE);

    DetectEngineCtx *de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL) {
        goto end;
    }
    de_ctx->mpm_matcher = MPM_B2G;
    de_ctx->flags |= DE_QUIET;

    s = de_ctx->sig_list = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent\"; "
                                   "uricontent:\"foo\"; content:\"bar\"; sid:1;)");
    if (s == NULL) {
        goto end;
    }

    s = s->next = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent\"; "
                                   "uricontent:\"one\"; offset:1; depth:10; "
                                   "content:\"one\"; offset:1; depth:10; "
                                   "uricontent:\"two\"; distance:3; within: 4; "
                                   "content:\"two\"; distance:1; within: 4; "
                                   "uricontent:\"three\"; distance:1; within: 6; "
                                   "content:\"/three\"; distance:0; within: 7; "
                                   "sid:2;)");

    if (s == NULL) {
        goto end;
    }

    s = s->next = SigInit(de_ctx,"alert tcp any any -> any any (msg:"
                                   "\" Test uricontent\"; "
                                   "uricontent:\"one\"; offset:1; depth:10; "
                                   "uricontent:\"two\"; distance:1; within: 4; "
                                   "uricontent:\"six\"; distance:1; within: 6; "
                                   "sid:3;)");

    if (s == NULL) {
        goto end;
    }

    SigGroupBuild(de_ctx);
    DetectEngineThreadCtxInit(&th_v, (void *)de_ctx, (void *)&det_ctx);

    int r = AppLayerParse(NULL, &f, ALPROTO_HTTP, STREAM_TOSERVER, httpbuf1, httplen1);
    if (r != 0) {
        printf("toserver chunk 1 returned %" PRId32 ", expected 0: ", r);
        goto end;
    }

    /* do detect */
    SigMatchSignatures(&th_v, de_ctx, det_ctx, p);

    http_state = f.alstate;
    if (http_state == NULL) {
        printf("no http state: ");
        goto end;
    }

    if (PacketAlertCheck(p, 1)) {
        printf("sig: 1 alerted, but it should not:");
        goto end;
    } else if (PacketAlertCheck(p, 2)) {
        printf("sig: 2 alerted, but it should not:");
        goto end;
    } else if (PacketAlertCheck(p, 3)) {
        printf("sig: 3 alerted, but it should not:");
        goto end;
    }

    result = 1;
end:

    if (de_ctx != NULL) SigGroupCleanup(de_ctx);
    if (de_ctx != NULL) SigCleanSignatures(de_ctx);
    if (det_ctx != NULL) DetectEngineThreadCtxDeinit(&th_v, det_ctx);
    if (de_ctx != NULL) DetectEngineCtxFree(de_ctx);

    StreamTcpFreeConfig(TRUE);
    FLOW_DESTROY(&f);
    UTHFreePackets(&p, 1);
    return result;
}

/**
 * \test Test content for dce sig.
 */
int DetectUriSigTest08(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert udp any any -> any any "
                               "(msg:\"test\"; uricontent:\"\"; sid:238012;)");
    if (de_ctx->sig_list != NULL) {
        result = 0;
        goto end;
    }

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Test content for dce sig.
 */
int DetectUriSigTest09(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert udp any any -> any any "
                               "(msg:\"test\"; uricontent:\"; sid:238012;)");
    if (de_ctx->sig_list != NULL) {
        result = 0;
        goto end;
    }

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Test content for dce sig.
 */
int DetectUriSigTest10(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert udp any any -> any any "
                               "(msg:\"test\"; uricontent:\"boo; sid:238012;)");
    if (de_ctx->sig_list != NULL) {
        result = 0;
        goto end;
    }

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Test content for dce sig.
 */
int DetectUriSigTest11(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert udp any any -> any any "
                               "(msg:\"test\"; uricontent:boo\"; sid:238012;)");
    if (de_ctx->sig_list != NULL) {
        result = 0;
        goto end;
    }

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Parsing test
 */
int DetectUriSigTest12(void)
{
    DetectEngineCtx *de_ctx = NULL;
    DetectContentData *ud = 0;
    Signature *s = NULL;
    int result = 0;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    s = de_ctx->sig_list = SigInit(de_ctx,
                                   "alert udp any any -> any any "
                                   "(msg:\"test\"; uricontent:    !\"boo\"; sid:238012;)");
    if (de_ctx->sig_list == NULL) {
        printf("de_ctx->sig_list == NULL: ");
        goto end;
    }

    if (s->sm_lists_tail[DETECT_SM_LIST_UMATCH] == NULL || s->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx == NULL) {
        printf("de_ctx->pmatch_tail == NULL && de_ctx->pmatch_tail->ctx == NULL: ");
        goto end;
    }

    ud = (DetectContentData *)s->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    result = (strncmp("boo", (char *)ud->content, ud->content_len) == 0);

end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}


/**
 * \test Parsing test
 */
int DetectUriContentParseTest13(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert udp any any -> any any "
                               "(msg:\"test\"; uricontent:\"|\"; sid:1;)");
    if (de_ctx->sig_list != NULL) {
        result = 0;
        goto end;
    }

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Parsing test
 */
int DetectUriContentParseTest14(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert udp any any -> any any "
                               "(msg:\"test\"; uricontent:\"|af\"; sid:1;)");
    if (de_ctx->sig_list != NULL) {
        result = 0;
        goto end;
    }

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Parsing test
 */
int DetectUriContentParseTest15(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert udp any any -> any any "
                               "(msg:\"test\"; uricontent:\"af|\"; sid:1;)");
    if (de_ctx->sig_list != NULL) {
        result = 0;
        goto end;
    }

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Parsing test
 */
int DetectUriContentParseTest16(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert udp any any -> any any "
                               "(msg:\"test\"; uricontent:\"|af|\"; sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Parsing test
 */
int DetectUriContentParseTest17(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert udp any any -> any any "
                               "(msg:\"test\"; uricontent:\"aast|\"; sid:1;)");
    if (de_ctx->sig_list != NULL) {
        result = 0;
        goto end;
    }

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Parsing test
 */
int DetectUriContentParseTest18(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert udp any any -> any any "
                               "(msg:\"test\"; uricontent:\"aast|af\"; sid:1;)");
    if (de_ctx->sig_list != NULL) {
        result = 0;
        goto end;
    }

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Parsing test
 */
int DetectUriContentParseTest19(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert udp any any -> any any "
                               "(msg:\"test\"; uricontent:\"aast|af|\"; sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Parsing test
 */
int DetectUriContentParseTest20(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert udp any any -> any any "
                               "(msg:\"test\"; uricontent:\"|af|asdf\"; sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Parsing test
 */
int DetectUriContentParseTest21(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert udp any any -> any any "
                               "(msg:\"test\"; uricontent:\"|af|af|\"; sid:1;)");
    if (de_ctx->sig_list != NULL) {
        result = 0;
        goto end;
    }

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Parsing test
 */
int DetectUriContentParseTest22(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert udp any any -> any any "
                               "(msg:\"test\"; uricontent:\"|af|af|af\"; sid:1;)");
    if (de_ctx->sig_list != NULL) {
        result = 0;
        goto end;
    }

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

/**
 * \test Parsing test
 */
int DetectUriContentParseTest23(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 1;

    de_ctx = DetectEngineCtxInit();
    if (de_ctx == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx,
                               "alert udp any any -> any any "
                               "(msg:\"test\"; uricontent:\"|af|af|af|\"; sid:1;)");
    if (de_ctx->sig_list == NULL) {
        result = 0;
        goto end;
    }

 end:
    SigGroupCleanup(de_ctx);
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);

    return result;
}

int DetectUricontentSigTest08(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:\"one\"; content:\"one\"; http_uri; sid:1;)");
    if (de_ctx->sig_list == NULL) {
        printf("de_ctx->sig_list == NULL\n");
        goto end;
    }

    if (de_ctx->sig_list->sm_lists[DETECT_SM_LIST_PMATCH] == NULL) {
        printf("de_ctx->sig_list->sm_lists[DETECT_SM_LIST_PMATCH] == NULL\n");
        goto end;
    }

    if (de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH] == NULL) {
        printf("de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH] == NULL\n");
        goto end;
    }

    DetectContentData *cd = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (cd->id == ud->id)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectUricontentSigTest09(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:\"one\"; content:\"one\"; sid:1;)");
    if (de_ctx->sig_list == NULL) {
        printf("de_ctx->sig_list == NULL\n");
        goto end;
    }

    if (de_ctx->sig_list->sm_lists[DETECT_SM_LIST_PMATCH] == NULL) {
        printf("de_ctx->sig_list->sm_lists[DETECT_SM_LIST_PMATCH] == NULL\n");
        goto end;
    }

    if (de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH] == NULL) {
        printf("de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH] == NULL\n");
        goto end;
    }

    DetectContentData *cd = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx;
    DetectContentData *ud = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (cd->id == ud->id)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectUricontentSigTest10(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(uricontent:\"one\"; content:\"one\"; content:\"one\"; http_uri; "
                               "content:\"two\"; content:\"one\"; sid:1;)");
    if (de_ctx->sig_list == NULL) {
        printf("de_ctx->sig_list == NULL\n");
        goto end;
    }

    if (de_ctx->sig_list->sm_lists[DETECT_SM_LIST_PMATCH] == NULL) {
        printf("de_ctx->sig_list->sm_lists[DETECT_SM_LIST_PMATCH] == NULL\n");
        goto end;
    }

    if (de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH] == NULL) {
        printf("de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH] == NULL\n");
        goto end;
    }

    DetectContentData *cd1 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->prev->prev->ctx;
    DetectContentData *cd2 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->prev->ctx;
    DetectContentData *cd3 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx;
    DetectContentData *ud1 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    DetectContentData *ud2 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (cd1->id != 1 || cd2->id != 2 || cd3->id != 1 || ud1->id != 0 || ud2->id != 0)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectUricontentSigTest11(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:\"one\"; http_uri; content:\"one\"; uricontent:\"one\"; "
                               "content:\"two\"; content:\"one\"; sid:1;)");
    if (de_ctx->sig_list == NULL) {
        printf("de_ctx->sig_list == NULL\n");
        goto end;
    }

    if (de_ctx->sig_list->sm_lists[DETECT_SM_LIST_PMATCH] == NULL) {
        printf("de_ctx->sig_list->sm_lists[DETECT_SM_LIST_PMATCH] == NULL\n");
        goto end;
    }

    if (de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH] == NULL) {
        printf("de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH] == NULL\n");
        goto end;
    }

    DetectContentData *cd1 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->prev->prev->ctx;
    DetectContentData *cd2 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->prev->ctx;
    DetectContentData *cd3 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx;
    DetectContentData *ud1 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    DetectContentData *ud2 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (cd1->id != 1 || cd2->id != 2 || cd3->id != 1 || ud1->id != 0 || ud2->id != 0)
        goto end;

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

int DetectUricontentSigTest12(void)
{
    DetectEngineCtx *de_ctx = NULL;
    int result = 0;

    if ( (de_ctx = DetectEngineCtxInit()) == NULL)
        goto end;

    de_ctx->flags |= DE_QUIET;
    de_ctx->sig_list = SigInit(de_ctx, "alert icmp any any -> any any "
                               "(content:\"one\"; http_uri; content:\"one\"; uricontent:\"one\"; "
                               "content:\"two\"; content:\"one\"; http_uri; content:\"one\"; "
                               "uricontent:\"one\"; uricontent: \"two\"; "
                               "content:\"one\"; content:\"three\"; sid:1;)");
    if (de_ctx->sig_list == NULL) {
        printf("de_ctx->sig_list == NULL\n");
        goto end;
    }

    if (de_ctx->sig_list->sm_lists[DETECT_SM_LIST_PMATCH] == NULL) {
        printf("de_ctx->sig_list->sm_lists[DETECT_SM_LIST_PMATCH] == NULL\n");
        goto end;
    }

    if (de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH] == NULL) {
        printf("de_ctx->sig_list->sm_lists[DETECT_SM_LIST_UMATCH] == NULL\n");
        goto end;
    }

    DetectContentData *cd1 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->prev->prev->prev->prev->ctx;
    DetectContentData *cd2 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->prev->prev->prev->ctx;
    DetectContentData *cd3 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->prev->prev->ctx;
    DetectContentData *cd4 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->prev->ctx;
    DetectContentData *cd5 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_PMATCH]->ctx;
    DetectContentData *ud1 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->prev->prev->prev->ctx;
    DetectContentData *ud2 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->prev->prev->ctx;
    DetectContentData *ud3 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->prev->ctx;
    DetectContentData *ud4 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->prev->ctx;
    DetectContentData *ud5 = de_ctx->sig_list->sm_lists_tail[DETECT_SM_LIST_UMATCH]->ctx;
    if (cd1->id != 1 || cd2->id != 2 || cd3->id != 1 || cd4->id != 1 || cd5->id != 4 ||
        ud1->id != 0 || ud2->id != 0 || ud3->id != 0 || ud4->id != 0 || ud5->id != 3) {
        goto end;
    }

    result = 1;

 end:
    SigCleanSignatures(de_ctx);
    DetectEngineCtxFree(de_ctx);
    return result;
}

#endif /* UNITTESTS */

void HttpUriRegisterTests(void) {
#ifdef UNITTESTS
    UtRegisterTest("HTTPUriTest01", HTTPUriTest01, 1);
    UtRegisterTest("HTTPUriTest02", HTTPUriTest02, 1);
    UtRegisterTest("HTTPUriTest03", HTTPUriTest03, 1);
    UtRegisterTest("HTTPUriTest04", HTTPUriTest04, 1);

    UtRegisterTest("DetectUriSigTest01", DetectUriSigTest01, 1);
    UtRegisterTest("DetectUriSigTest02", DetectUriSigTest02, 1);
    UtRegisterTest("DetectUriSigTest03", DetectUriSigTest03, 1);
    UtRegisterTest("DetectUriSigTest04 - Modifiers", DetectUriSigTest04, 1);
    UtRegisterTest("DetectUriSigTest05 - Inspection", DetectUriSigTest05, 1);
    UtRegisterTest("DetectUriSigTest06 - Inspection", DetectUriSigTest06, 1);
    UtRegisterTest("DetectUriSigTest07 - Inspection", DetectUriSigTest07, 1);
    UtRegisterTest("DetectUriSigTest08", DetectUriSigTest08, 1);
    UtRegisterTest("DetectUriSigTest09", DetectUriSigTest09, 1);
    UtRegisterTest("DetectUriSigTest10", DetectUriSigTest10, 1);
    UtRegisterTest("DetectUriSigTest11", DetectUriSigTest11, 1);
    UtRegisterTest("DetectUriSigTest12", DetectUriSigTest12, 1);

    UtRegisterTest("DetectUriContentParseTest13", DetectUriContentParseTest13, 1);
    UtRegisterTest("DetectUriContentParseTest14", DetectUriContentParseTest14, 1);
    UtRegisterTest("DetectUriContentParseTest15", DetectUriContentParseTest15, 1);
    UtRegisterTest("DetectUriContentParseTest16", DetectUriContentParseTest16, 1);
    UtRegisterTest("DetectUriContentParseTest17", DetectUriContentParseTest17, 1);
    UtRegisterTest("DetectUriContentParseTest18", DetectUriContentParseTest18, 1);
    UtRegisterTest("DetectUriContentParseTest19", DetectUriContentParseTest19, 1);
    UtRegisterTest("DetectUriContentParseTest20", DetectUriContentParseTest20, 1);
    UtRegisterTest("DetectUriContentParseTest21", DetectUriContentParseTest21, 1);
    UtRegisterTest("DetectUriContentParseTest22", DetectUriContentParseTest22, 1);
    UtRegisterTest("DetectUriContentParseTest23", DetectUriContentParseTest23, 1);
    UtRegisterTest("DetectUricontentSigTest08", DetectUricontentSigTest08, 1);
    UtRegisterTest("DetectUricontentSigTest09", DetectUricontentSigTest09, 1);
    UtRegisterTest("DetectUricontentSigTest10", DetectUricontentSigTest10, 1);
    UtRegisterTest("DetectUricontentSigTest11", DetectUricontentSigTest11, 1);
    UtRegisterTest("DetectUricontentSigTest12", DetectUricontentSigTest12, 1);
#endif /* UNITTESTS */
}
