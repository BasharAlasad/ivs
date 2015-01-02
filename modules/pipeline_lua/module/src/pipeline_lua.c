/****************************************************************
 *
 *        Copyright 2014, Big Switch Networks, Inc.
 *
 * Licensed under the Eclipse Public License, Version 1.0 (the
 * "License"); you may not use this file except in compliance
 * with the License. You may obtain a copy of the License at
 *
 *        http://www.eclipse.org/legal/epl-v10.html
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND,
 * either express or implied. See the License for the specific
 * language governing permissions and limitations under the
 * License.
 *
 ****************************************************************/

#include <pipeline/pipeline.h>
#include <stdlib.h>

#include <ivs/ivs.h>
#include <loci/loci.h>
#include <OVSDriver/ovsdriver.h>
#include <indigo/indigo.h>
#include <indigo/of_state_manager.h>
#include <lua.h>
#include <lualib.h>
#include <lauxlib.h>
#include <pthread.h>

#include "pipeline_lua_int.h"

#define AIM_LOG_MODULE_NAME pipeline_lua
#include <AIM/aim_log.h>

AIM_LOG_STRUCT_DEFINE(AIM_LOG_OPTIONS_DEFAULT, AIM_LOG_BITS_DEFAULT, NULL, 0);

/* Per-packet information shared with Lua */
struct context {
    struct xbuf *stats;
    struct action_context *actx;
    struct fields fields;
};

struct upload_chunk {
    of_str64_t filename;
    uint32_t size;
    char data[];
};

static void pipeline_lua_finish(void);
static indigo_core_listener_result_t message_listener(indigo_cxn_id_t cxn_id, of_object_t *msg);
static void commit_lua_upload(indigo_cxn_id_t cxn_id, of_object_t *msg);
static void cleanup_lua_upload(void);

static lua_State *lua;
static struct context context;
static int process_ref;

/* List of struct upload_chunk */
struct xbuf upload_chunks;

static void
pipeline_lua_init(const char *name)
{
    indigo_core_message_listener_register(message_listener);
    xbuf_init(&upload_chunks);

    lua = luaL_newstate();
    if (lua == NULL) {
        AIM_DIE("failed to allocate Lua state");
    }

    luaL_openlibs(lua);

    /* Give Lua a pointer to the static context struct */
    lua_pushlightuserdata(lua, &context);
    lua_setglobal(lua, "_context");

    /* Give Lua the names of all fields */
    lua_newtable(lua);
    int i = 0;
    while (pipeline_lua_field_names[i]) {
        lua_pushstring(lua, pipeline_lua_field_names[i]);
        lua_rawseti(lua, -2, i+1);
        i++;
    }
    lua_setglobal(lua, "field_names");

    lua_pushcfunction(lua, pipeline_lua_table_register);
    lua_setglobal(lua, "register_table");

    const struct builtin_lua *builtin_lua;
    for (builtin_lua = &pipeline_lua_builtin_lua[0];
            builtin_lua->name; builtin_lua++) {
        AIM_LOG_VERBOSE("Loading builtin Lua code %s", builtin_lua->name);

        /* Parse */
        if (luaL_loadbuffer(lua, builtin_lua->start,
                builtin_lua->end-builtin_lua->start,
                builtin_lua->name) != 0) {
            AIM_DIE("Failed to load built-in Lua code %s: %s",
                    builtin_lua->name, lua_tostring(lua, -1));
        }

        /* Execute */
        if (lua_pcall(lua, 0, 0, 0) != 0) {
            AIM_DIE("Failed to execute built-in Lua code %s: %s",
                    builtin_lua->name, lua_tostring(lua, -1));
        }
    }

    /* Store a reference to process() so we can efficiently retrieve it */
    lua_getglobal(lua, "process");
    AIM_ASSERT(lua_isfunction(lua, -1));
    process_ref = luaL_ref(lua, LUA_REGISTRYINDEX);
}

static void
pipeline_lua_finish(void)
{
    lua_close(lua);
    lua = NULL;

    indigo_core_message_listener_unregister(message_listener);
    cleanup_lua_upload();
    xbuf_cleanup(&upload_chunks);
}

indigo_error_t
pipeline_lua_process(struct ind_ovs_parsed_key *key,
                     struct xbuf *stats,
                     struct action_context *actx)
{
    pipeline_lua_fields_from_key(key, &context.fields);
    context.stats = stats;
    context.actx = actx;

    lua_rawgeti(lua, LUA_REGISTRYINDEX, process_ref);

    if (lua_pcall(lua, 0, 0, 0) != 0) {
        AIM_LOG_ERROR("Failed to execute script: %s", lua_tostring(lua, -1));
    }

    return INDIGO_ERROR_NONE;
}

static struct pipeline_ops pipeline_lua_ops = {
    .init = pipeline_lua_init,
    .finish = pipeline_lua_finish,
    .process = pipeline_lua_process,
};

static void
handle_lua_upload(indigo_cxn_id_t cxn_id, of_object_t *msg)
{
    of_octets_t data;
    uint16_t flags;
    of_str64_t filename;
    of_bsn_lua_upload_data_get(msg, &data);
    of_bsn_lua_upload_flags_get(msg, &flags);
    of_bsn_lua_upload_filename_get(msg, &filename);

    /* Ensure filename is null terminated */
    filename[63] = 0;

    /* TODO limit size */
    /* TODO concatenate consecutive messages with the same filename */

    if (data.bytes > 0) {
        struct upload_chunk *chunk = xbuf_reserve(&upload_chunks, sizeof(*chunk) + data.bytes);
        chunk->size = data.bytes;
        memcpy(chunk->filename, filename, sizeof(of_str64_t));
        memcpy(chunk->data, data.data, data.bytes);

        AIM_LOG_VERBOSE("Uploaded Lua chunk %s, %u bytes", chunk->filename, chunk->size);
    }

    if (!(flags & OFP_BSN_LUA_UPLOAD_MORE)) {
        commit_lua_upload(cxn_id, msg);
    }
}

static void
commit_lua_upload(indigo_cxn_id_t cxn_id, of_object_t *msg)
{
    // TODO create new VM and clean up old one
    // TODO skip if code is identical

    uint32_t offset = 0;
    while (offset < xbuf_length(&upload_chunks)) {
        struct upload_chunk *chunk = xbuf_data(&upload_chunks) + offset;
        offset += sizeof(*chunk) + chunk->size;

        AIM_LOG_VERBOSE("Loading Lua chunk %s, %u bytes", chunk->filename, chunk->size);

        if (luaL_loadbuffer(lua, chunk->data, chunk->size, chunk->filename) != 0) {
            AIM_LOG_ERROR("Failed to load code: %s", lua_tostring(lua, -1));
            indigo_cxn_send_error_reply(
                cxn_id, msg, OF_ERROR_TYPE_BAD_REQUEST, OF_REQUEST_FAILED_EPERM);
            goto cleanup;
        }

        /* Set the environment of the new chunk to the sandbox */
        lua_getglobal(lua, "sandbox");
        lua_setfenv(lua, -2);

        if (lua_pcall(lua, 0, 0, 0) != 0) {
            AIM_LOG_ERROR("Failed to execute code %s: %s", chunk->filename, lua_tostring(lua, -1));
            indigo_cxn_send_error_reply(
                cxn_id, msg, OF_ERROR_TYPE_BAD_REQUEST, OF_REQUEST_FAILED_EPERM);
            goto cleanup;
        }
    }

cleanup:
    cleanup_lua_upload();
    return;
}

static void
cleanup_lua_upload(void)
{
    xbuf_reset(&upload_chunks);
}

static indigo_core_listener_result_t
message_listener(indigo_cxn_id_t cxn_id, of_object_t *msg)
{
    switch (msg->object_id) {
    case OF_BSN_LUA_UPLOAD:
        handle_lua_upload(cxn_id, msg);
        return INDIGO_CORE_LISTENER_RESULT_DROP;

    default:
        return INDIGO_CORE_LISTENER_RESULT_PASS;
    }
}

/* Called by Lua to log a message */
void
pipeline_lua_log(const char *str)
{
    AIM_LOG_VERBOSE("%s", str);
}

void
__pipeline_lua_module_init__(void)
{
    AIM_LOG_STRUCT_REGISTER();
    pipeline_register("lua", &pipeline_lua_ops);
}
