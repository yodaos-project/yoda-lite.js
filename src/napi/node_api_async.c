/* Copyright 2019-present Samsung Electronics Co., Ltd. and other contributors
 * Copyright 2018-present Rokid Co., Ltd. and other contributors
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "internal/node_api_internal.h"
#include "rtnode-module-process.h"

static void on_work(rtev_worker_t *req) {
  rtnode_async_work_t* async_work = (rtnode_async_work_t*)req->data;
  if (async_work && async_work->execute != NULL) {
    async_work->execute(async_work->env, async_work->data);
  }
}

static void on_work_done(rtev_worker_t* req) {
  rtnode_async_work_t* async_work = (rtnode_async_work_t*)req->data;
  RTNODE_ASSERT(async_work != NULL);
  napi_status cb_status = napi_ok;

  napi_env env = async_work->env;
  napi_async_complete_callback complete = async_work->complete;
  void* data = async_work->data;

  if (complete != NULL) {
    jerryx_handle_scope scope;
    jerryx_open_handle_scope(&scope);
    /**
     * napi_async_work could be deleted by invocation of `napi_delete_asyncwork`
     * in its complete callback.
     */
    complete(env, cb_status, data);
    jerryx_close_handle_scope(scope);

    if (rtnode_napi_is_exception_pending(env)) {
      jerry_value_t jval_err;
      jval_err = rtnode_napi_env_get_and_clear_exception(env);
      if (jval_err == (uintptr_t)NULL) {
        jval_err = rtnode_napi_env_get_and_clear_fatal_exception(env);
      }

      /** Argument cannot have error flag */
      rtnode_on_fatal_error(jerry_get_value_from_error(jval_err, false), NULL);
      jerry_release_value(jval_err);
    }
  }

  rtnode_run_next_tick();
}

static void on_work_close(rtev_watcher_t *worker) {
  rtnode_free(worker);
}

napi_status napi_create_async_work(napi_env env, napi_value async_resource,
                                   napi_value async_resource_name,
                                   napi_async_execute_callback execute,
                                   napi_async_complete_callback complete,
                                   void* data, napi_async_work* result) {
  NAPI_TRY_ENV(env);
  NAPI_WEAK_ASSERT(napi_invalid_arg, result != NULL);
  NAPI_WEAK_ASSERT(napi_invalid_arg, execute != NULL);
  NAPI_WEAK_ASSERT(napi_invalid_arg, complete != NULL);

  rtnode_async_work_t* async_work = rtnode_malloc(sizeof(rtnode_async_work_t));
  rtev_worker_t* work_req = &async_work->work_req;

  async_work->env = env;
  async_work->async_resource = async_resource;
  async_work->async_resource_name = async_resource_name;
  async_work->execute = execute;
  async_work->complete = complete;
  async_work->data = data;

  work_req->data = async_work;

  NAPI_ASSIGN(result, (napi_async_work)work_req);
  NAPI_RETURN(napi_ok);
}

napi_status napi_delete_async_work(napi_env env, napi_async_work work) {
  return napi_cancel_async_work(env, work);
}

napi_status napi_queue_async_work(napi_env env, napi_async_work work) {
  NAPI_TRY_ENV(env);
  rtev_ctx_t *ctx = rtnode_get_context()->rtev;

  rtev_worker_t* work_req = (rtev_worker_t*)work;

  int status = rtev_worker_start(ctx, work_req, on_work, on_work_done, on_work_close);
  if (status != 0) {
    const char* err_name = strerror(status);
    NAPI_RETURN_WITH_MSG(napi_generic_failure, err_name);
  }
  NAPI_RETURN(napi_ok);
}

napi_status napi_cancel_async_work(napi_env env, napi_async_work work) {
  NAPI_TRY_ENV(env);
  rtev_watcher_t* work_req = (rtev_watcher_t*)work;
  int status = rtev_watcher_close(work_req);
  if (status != 0) {
    const char* err_name = strerror(status);
    NAPI_RETURN_WITH_MSG(napi_generic_failure, err_name);
  }
  NAPI_RETURN(napi_ok);
}

napi_status napi_async_init(napi_env env, napi_value async_resource,
                            napi_value async_resource_name,
                            napi_async_context* result) {
  NAPI_TRY_ENV(env);

  rtnode_async_context_t* ctx = rtnode_malloc(sizeof(rtnode_async_context_t));
  ctx->env = env;
  ctx->async_resource = async_resource;
  ctx->async_resource_name = async_resource_name;

  NAPI_ASSIGN(result, (napi_async_context)ctx);
  return napi_ok;
}

napi_status napi_async_destroy(napi_env env, napi_async_context async_context) {
  NAPI_TRY_ENV(env);

  rtnode_async_context_t* ctx = (rtnode_async_context_t*)async_context;
  rtnode_free(ctx);

  return napi_ok;
}

napi_status napi_make_callback(napi_env env, napi_async_context async_context,
                               napi_value recv, napi_value func, size_t argc,
                               const napi_value* argv, napi_value* result) {
  NAPI_TRY_ENV(env);

  napi_status status = napi_call_function(env, recv, func, argc, argv, result);
  if (!rtnode_napi_is_exception_pending(env)) {
    rtnode_run_next_tick();
  } else {
    // In this case explicit napi_async_destroy calls won't be executed, so
    // free the context manually.
    rtnode_free(async_context);
  }

  return status;
}
