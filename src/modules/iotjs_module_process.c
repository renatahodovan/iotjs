/* Copyright 2015-present Samsung Electronics Co., Ltd. and other contributors
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

#include "iotjs_def.h"
#include "iotjs_js.h"
#include "jerryscript-debugger.h"

#include <stdlib.h>


JHANDLER_FUNCTION(Binding) {
  DJHANDLER_CHECK_ARGS(1, number);

  ModuleKind module_kind = (ModuleKind)JHANDLER_GET_ARG(0, number);

  const iotjs_jval_t jmodule =
      *iotjs_module_initialize_if_necessary(module_kind);

  iotjs_jhandler_return_jval(jhandler, jmodule);
}


static iotjs_jval_t WrapEval(const char* name, size_t name_len,
                             const char* source, size_t length, bool* throws) {
  static const char* args = "exports, require, module";
  jerry_value_t res =
      jerry_parse_function((const jerry_char_t*)name, name_len,
                           (const jerry_char_t*)args, strlen(args),
                           (const jerry_char_t*)source, length, false);

  *throws = jerry_value_has_error_flag(res);

  return res;
}


JHANDLER_FUNCTION(Compile) {
  DJHANDLER_CHECK_ARGS(2, string, string);

  iotjs_string_t file = JHANDLER_GET_ARG(0, string);
  iotjs_string_t source = JHANDLER_GET_ARG(1, string);

  const char* filename = iotjs_string_data(&file);
  const iotjs_environment_t* env = iotjs_environment_get();

  if (iotjs_environment_config(env)->debugger != NULL) {
    jerry_debugger_stop();
  }

  bool throws;
  iotjs_jval_t jres =
      WrapEval(filename, strlen(filename), iotjs_string_data(&source),
               iotjs_string_size(&source), &throws);

  if (!throws) {
    iotjs_jhandler_return_jval(jhandler, jres);
  } else {
    iotjs_jhandler_throw(jhandler, jres);
  }

  iotjs_string_destroy(&file);
  iotjs_string_destroy(&source);
  jerry_release_value(jres);
}


// Callback function for DebuggerSourceCompile
static jerry_value_t wait_for_source_callback(
    const jerry_char_t* resource_name_p, size_t resource_name_size,
    const jerry_char_t* source_p, size_t size, void* jhandler) {
  char* filename = (char*)resource_name_p;
  iotjs_string_t source =
      iotjs_string_create_with_buffer((char*)source_p, size);

  jerry_debugger_stop();

  bool throws;
  iotjs_jval_t jres =
      WrapEval(filename, resource_name_size, iotjs_string_data(&source),
               iotjs_string_size(&source), &throws);

  if (!throws) {
    iotjs_jhandler_return_jval(jhandler, jres);
  } else {
    iotjs_jhandler_throw(jhandler, jres);
  }

  jerry_release_value(jres);
  return jerry_create_undefined();
}


// Compile source received from debugger
JHANDLER_FUNCTION(DebuggerSourceCompile) {
  jerry_value_t res;
  jerry_debugger_wait_for_client_source(wait_for_source_callback, jhandler,
                                        &res);

  jerry_release_value(res);
}


JHANDLER_FUNCTION(CompileNativePtr) {
  DJHANDLER_CHECK_ARGS(1, string);

  iotjs_string_t id = JHANDLER_GET_ARG(0, string);
  const char* name = iotjs_string_data(&id);

  int i = 0;
  while (natives[i].name != NULL) {
    if (!strcmp(natives[i].name, name)) {
      break;
    }

    i++;
  }

  if (natives[i].name != NULL) {
    bool throws;
#ifdef ENABLE_SNAPSHOT
    jerry_value_t jres = iotjs_exec_snapshot(natives[i].idx, &throws);
#else
    iotjs_jval_t jres =
        WrapEval(name, iotjs_string_size(&id), (const char*)natives[i].code,
                 natives[i].length, &throws);
#endif

    if (!throws) {
      iotjs_jhandler_return_jval(jhandler, jres);
    } else {
      iotjs_jhandler_throw(jhandler, jres);
    }
    jerry_release_value(jres);
  } else {
    iotjs_jval_t jerror = iotjs_jval_create_error("Unknown native module");
    iotjs_jhandler_throw(jhandler, jerror);
    jerry_release_value(jerror);
  }

  iotjs_string_destroy(&id);
}


JHANDLER_FUNCTION(ReadSource) {
  DJHANDLER_CHECK_ARGS(1, string);

  iotjs_string_t path = JHANDLER_GET_ARG(0, string);
  iotjs_string_t code = iotjs_file_read(iotjs_string_data(&path));

  iotjs_jhandler_return_string(jhandler, &code);

  iotjs_string_destroy(&path);
  iotjs_string_destroy(&code);
}


JHANDLER_FUNCTION(Cwd) {
  char path[IOTJS_MAX_PATH_SIZE];
  size_t size_path = sizeof(path);
  int err = uv_cwd(path, &size_path);
  if (err) {
    JHANDLER_THROW(COMMON, "cwd error");
    return;
  }
  iotjs_jhandler_return_string_raw(jhandler, path);
}

JHANDLER_FUNCTION(Chdir) {
  DJHANDLER_CHECK_ARGS(1, string);

  iotjs_string_t path = JHANDLER_GET_ARG(0, string);
  int err = uv_chdir(iotjs_string_data(&path));

  if (err) {
    iotjs_string_destroy(&path);
    JHANDLER_THROW(COMMON, "chdir error");
    return;
  }

  iotjs_string_destroy(&path);
}


JHANDLER_FUNCTION(DoExit) {
  iotjs_environment_t* env = iotjs_environment_get();

  if (!iotjs_environment_is_exiting(env)) {
    DJHANDLER_CHECK_ARGS(1, number);
    int exit_code = JHANDLER_GET_ARG(0, number);

    iotjs_set_process_exitcode(exit_code);
    iotjs_environment_go_state_exiting(env);
  }
}


void SetNativeSources(iotjs_jval_t native_sources) {
  for (int i = 0; natives[i].name; i++) {
    iotjs_jval_set_property_jval(native_sources, natives[i].name,
                                 jerry_create_boolean(true));
  }
}


static void SetProcessEnv(iotjs_jval_t process) {
  const char *homedir, *iotjspath, *iotjsenv;

  homedir = getenv("HOME");
  if (homedir == NULL) {
    homedir = "";
  }

  iotjspath = getenv("IOTJS_PATH");
  if (iotjspath == NULL) {
#if defined(__NUTTX__) || defined(__TIZENRT__)
    iotjspath = "/mnt/sdcard";
#else
    iotjspath = "";
#endif
  }

#if defined(EXPERIMENTAL)
  iotjsenv = "experimental";
#else
  iotjsenv = "";
#endif

  iotjs_jval_t env = iotjs_jval_create_object();
  iotjs_jval_set_property_string_raw(env, IOTJS_MAGIC_STRING_HOME, homedir);
  iotjs_jval_set_property_string_raw(env, IOTJS_MAGIC_STRING_IOTJS_PATH,
                                     iotjspath);
  iotjs_jval_set_property_string_raw(env, IOTJS_MAGIC_STRING_IOTJS_ENV,
                                     iotjsenv);

  iotjs_jval_set_property_jval(process, IOTJS_MAGIC_STRING_ENV, env);

  jerry_release_value(env);
}


static void SetProcessIotjs(iotjs_jval_t process) {
  // IoT.js specific
  iotjs_jval_t iotjs = iotjs_jval_create_object();
  iotjs_jval_set_property_jval(process, IOTJS_MAGIC_STRING_IOTJS, iotjs);

  iotjs_jval_set_property_string_raw(iotjs, IOTJS_MAGIC_STRING_BOARD,
                                     TOSTRING(TARGET_BOARD));
  jerry_release_value(iotjs);
}


static void SetProcessArgv(iotjs_jval_t process) {
  const iotjs_environment_t* env = iotjs_environment_get();
  uint32_t argc = iotjs_environment_argc(env);

  iotjs_jval_t argv = iotjs_jval_create_array(argc);

  for (uint32_t i = 0; i < argc; ++i) {
    const char* argvi = iotjs_environment_argv(env, i);
    iotjs_jval_t arg = iotjs_jval_create_string_raw(argvi);
    iotjs_jval_set_property_by_index(argv, i, arg);
    jerry_release_value(arg);
  }
  iotjs_jval_set_property_jval(process, IOTJS_MAGIC_STRING_ARGV, argv);

  jerry_release_value(argv);
}


iotjs_jval_t InitProcess() {
  iotjs_jval_t process = iotjs_jval_create_object();

  iotjs_jval_set_method(process, IOTJS_MAGIC_STRING_BINDING, Binding);
  iotjs_jval_set_method(process, IOTJS_MAGIC_STRING_COMPILE, Compile);
  iotjs_jval_set_method(process, IOTJS_MAGIC_STRING_COMPILENATIVEPTR,
                        CompileNativePtr);
  iotjs_jval_set_method(process, IOTJS_MAGIC_STRING_READSOURCE, ReadSource);
  iotjs_jval_set_method(process, IOTJS_MAGIC_STRING_CWD, Cwd);
  iotjs_jval_set_method(process, IOTJS_MAGIC_STRING_CHDIR, Chdir);
  iotjs_jval_set_method(process, IOTJS_MAGIC_STRING_DEBUGGER_SOURCE_COMPILE,
                        DebuggerSourceCompile);
  iotjs_jval_set_method(process, IOTJS_MAGIC_STRING_DOEXIT, DoExit);
  SetProcessEnv(process);

  // process.native_sources
  iotjs_jval_t native_sources = iotjs_jval_create_object();
  SetNativeSources(native_sources);
  iotjs_jval_set_property_jval(process, IOTJS_MAGIC_STRING_NATIVE_SOURCES,
                               native_sources);
  jerry_release_value(native_sources);

  // process.platform
  iotjs_jval_set_property_string_raw(process, IOTJS_MAGIC_STRING_PLATFORM,
                                     TARGET_OS);

  // process.arch
  iotjs_jval_set_property_string_raw(process, IOTJS_MAGIC_STRING_ARCH,
                                     TARGET_ARCH);

  // process.version
  iotjs_jval_set_property_string_raw(process, IOTJS_MAGIC_STRING_VERSION,
                                     IOTJS_VERSION);

  // Set iotjs
  SetProcessIotjs(process);
  bool wait_source;
  if (iotjs_environment_config(iotjs_environment_get())->debugger != NULL) {
    wait_source = iotjs_environment_config(iotjs_environment_get())
                      ->debugger->wait_source;
  } else {
    wait_source = false;
  }

  if (!wait_source) {
    SetProcessArgv(process);
  }

  iotjs_jval_t wait_source_val = jerry_create_boolean(wait_source);
  iotjs_jval_set_property_jval(process, IOTJS_MAGIC_STRING_DEBUGGER_WAIT_SOURCE,
                               wait_source_val);

  // Binding module id.
  iotjs_jval_t jbinding =
      iotjs_jval_get_property(process, IOTJS_MAGIC_STRING_BINDING);

#define ENUMDEF_MODULE_LIST(upper, Camel, lower) \
  iotjs_jval_set_property_number(jbinding, #lower, MODULE_##upper);

  MAP_MODULE_LIST(ENUMDEF_MODULE_LIST)

#undef ENUMDEF_MODULE_LIST

  jerry_release_value(wait_source_val);
  jerry_release_value(jbinding);

  return process;
}
