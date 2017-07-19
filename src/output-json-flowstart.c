/* Copyright (C) 2007-2017 Open Information Security Foundation
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
 * \author Martin Petracek <martin.petracek@nic.cz>
 *
 * JSON Flow start log module to log start of flows
 *
 */

#include "suricata-common.h"
#include "debug.h"
#include "detect.h"
#include "flow.h"
#include "conf.h"

#include "threads.h"
#include "tm-threads.h"
#include "threadvars.h"
#include "util-debug.h"

#include "decode-ipv4.h"
#include "detect-parse.h"
#include "detect-engine.h"
#include "detect-engine-mpm.h"
#include "detect-reference.h"

#include "output.h"
#include "output-json.h"
#include "output-json-alert.h"
#include "output-json-flowstart.h"

#include "util-unittest.h"
#include "util-unittest-helper.h"
#include "util-classification-config.h"
#include "util-privs.h"
#include "util-print.h"
#include "util-proto-name.h"
#include "util-logopenfile.h"
#include "util-time.h"
#include "util-buffer.h"

#include <net/if.h> /*if_indextoname*/

#define MODULE_NAME "JsonFlowstartLog"

#ifdef HAVE_LIBJANSSON

typedef struct JsonFlowstartOutputCtx_ {
    LogFileCtx *file_ctx;
} JsonFlowstartOutputCtx;

typedef struct JsonFlowstartLogThread_ {
    JsonFlowstartOutputCtx *flowstart_ctx;
    MemBuffer *buffer;
} JsonFlowstartLogThread;

/**
 * \brief   Log the dropped packets in netfilter format when engine is running
 *          in inline mode
 *
 * \param tv    Pointer the current thread variables
 * \param p     Pointer the packet which is being logged
 *
 * \return return TM_EODE_OK on success
 */
static int FlowstartLogJSON (JsonFlowstartLogThread *aft, const Packet *p)
{
    json_t *js = CreateJSONHeader((Packet *)p, 0, "flow_start");//TODO const
    if (unlikely(js == NULL))
        return TM_ECODE_OK;
#ifdef NFQ
    if (p->nfq_v.ifi != 0) {
        char name[IFNAMSIZ];
        if_indextoname (p->nfq_v.ifi, name);
        json_object_set_new(js, "in_dev", json_string(name));
    }
#endif /*NFQ*/
    MemBufferReset(aft->buffer);
    OutputJSONBuffer(js, aft->flowstart_ctx->file_ctx, &aft->buffer);
    json_object_clear(js);
    json_decref(js);

    return TM_ECODE_OK;
}

#define OUTPUT_BUFFER_SIZE 65535
static TmEcode JsonFlowstartLogThreadInit(ThreadVars *t, void *initdata, void **data)
{
    JsonFlowstartLogThread *aft = SCMalloc(sizeof(JsonFlowstartLogThread));
    if (unlikely(aft == NULL))
        return TM_ECODE_FAILED;
    memset(aft, 0, sizeof(*aft));

    if(initdata == NULL)
    {
        SCLogDebug("Error getting context for EveLogDrop.  \"initdata\" argument NULL");
        SCFree(aft);
        return TM_ECODE_FAILED;
    }

    aft->buffer = MemBufferCreateNew(OUTPUT_BUFFER_SIZE);
    if (aft->buffer == NULL) {
        SCFree(aft);
        return TM_ECODE_FAILED;
    }

    /** Use the Ouptut Context (file pointer and mutex) */
    aft->flowstart_ctx = ((OutputCtx *)initdata)->data;

    *data = (void *)aft;
    return TM_ECODE_OK;
}

static TmEcode JsonFlowstartLogThreadDeinit(ThreadVars *t, void *data)
{
    JsonFlowstartLogThread *aft = (JsonFlowstartLogThread *)data;
    if (aft == NULL) {
        return TM_ECODE_OK;
    }

    MemBufferFree(aft->buffer);

    /* clear memory */
    memset(aft, 0, sizeof(*aft));

    SCFree(aft);
    return TM_ECODE_OK;
}

static void JsonFlowstartOutputCtxFree(JsonFlowstartOutputCtx *flowstart_ctx)
{
    if (flowstart_ctx != NULL) {
        if (flowstart_ctx->file_ctx != NULL)
            LogFileFreeCtx(flowstart_ctx->file_ctx);
        SCFree(flowstart_ctx);
    }
}

static void JsonFlowstartLogDeInitCtx(OutputCtx *output_ctx)
{

    JsonFlowstartOutputCtx *flowstart_ctx = output_ctx->data;
    JsonFlowstartOutputCtxFree(flowstart_ctx);
    SCFree(output_ctx);
}

static void JsonFlowstartLogDeInitCtxSub(OutputCtx *output_ctx)
{

    JsonFlowstartOutputCtx *flowstart_ctx = output_ctx->data;
    SCFree(flowstart_ctx);
    SCLogDebug("cleaning up sub output_ctx %p", output_ctx);
    SCFree(output_ctx);
}

#define DEFAULT_LOG_FILENAME "flowstart.json"
static OutputCtx *JsonFlowstartLogInitCtx(ConfNode *conf)
{
    JsonFlowstartOutputCtx *flowstart_ctx = SCCalloc(1, sizeof(*flowstart_ctx));
    if (flowstart_ctx == NULL)
        return NULL;

    flowstart_ctx->file_ctx = LogFileNewCtx();
    if (flowstart_ctx->file_ctx == NULL) {
        JsonFlowstartOutputCtxFree(flowstart_ctx);
        return NULL;
    }

    if (SCConfLogOpenGeneric(conf, flowstart_ctx->file_ctx, DEFAULT_LOG_FILENAME, 1) < 0) {
        JsonFlowstartOutputCtxFree(flowstart_ctx);
        return NULL;
    }

    OutputCtx *output_ctx = SCCalloc(1, sizeof(OutputCtx));
    if (unlikely(output_ctx == NULL)) {
        JsonFlowstartOutputCtxFree(flowstart_ctx);
        return NULL;
    }

    output_ctx->data = flowstart_ctx;
    output_ctx->DeInit = JsonFlowstartLogDeInitCtx;
    return output_ctx;
}

static OutputCtx *JsonFlowstartLogInitCtxSub(ConfNode *conf, OutputCtx *parent_ctx)
{
    
    OutputJsonCtx *ojc = parent_ctx->data;

    JsonFlowstartOutputCtx *flowstart_ctx = SCCalloc(1, sizeof(*flowstart_ctx));
    if (flowstart_ctx == NULL)
        return NULL;

    OutputCtx *output_ctx = SCCalloc(1, sizeof(OutputCtx));
    if (unlikely(output_ctx == NULL)) {
        JsonFlowstartOutputCtxFree(flowstart_ctx);
        return NULL;
    }

    flowstart_ctx->file_ctx = ojc->file_ctx;

    output_ctx->data = flowstart_ctx;
    output_ctx->DeInit = JsonFlowstartLogDeInitCtxSub;
    return output_ctx;
}

/**
 * \brief   Log the dropped packets when engine is running in inline mode
 *
 * \param tv    Pointer the current thread variables
 * \param data  Pointer to the droplog struct
 * \param p     Pointer the packet which is being logged
 *
 * \retval 0 on succes
 */
static int JsonFlowstartLogger(ThreadVars *tv, void *thread_data, const Packet *p)
{
    JsonFlowstartLogThread *td = thread_data;
    int r = FlowstartLogJSON(td, p);
    if (r < 0)
        return -1;
    return 0;
}


/**
 * \brief Check if we need to drop-log this packet
 *
 * \param tv    Pointer the current thread variables
 * \param p     Pointer the packet which is tested
 *
 * \retval bool TRUE or FALSE
 */
static int JsonFlowstartLogCondition(ThreadVars *tv, const Packet *p)
{
    if (!EngineModeIsIPS()) {
        SCLogDebug("engine is not running in inline mode, so returning");
        return FALSE;
    }
    if (PKT_IS_PSEUDOPKT(p)) {
        SCLogDebug("drop log doesn't log pseudo packets");
        return FALSE;
    }
    
    if (p->flow == NULL) return FALSE;
    if ((p->flow->todstpktcnt + p->flow->tosrcpktcnt) == 1) return TRUE;
    return FALSE;
}

void JsonFlowstartLogRegister (void)
{
    OutputRegisterPacketModule(LOGGER_JSON_FLOWSTART, MODULE_NAME, "flow_start-json-log",
        JsonFlowstartLogInitCtx, JsonFlowstartLogger, JsonFlowstartLogCondition,
        JsonFlowstartLogThreadInit, JsonFlowstartLogThreadDeinit, NULL);
    OutputRegisterPacketSubModule(LOGGER_JSON_FLOWSTART, "eve-log", MODULE_NAME,
        "eve-log.flow_start", JsonFlowstartLogInitCtxSub, JsonFlowstartLogger,
        JsonFlowstartLogCondition, JsonFlowstartLogThreadInit, JsonFlowstartLogThreadDeinit,
        NULL);
}

#else

void JsonFlowstartLogRegister (void)
{
}

#endif
