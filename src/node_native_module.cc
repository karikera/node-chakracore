#include "node_native_module.h"
#include "node_errors.h"
#include "node_internals.h"

#include "src/jsrtutils.h"
#include "src/jsrtcontextshim.h"
#include "src/jsrtisolateshim.h"

#define NODEGATE_EXPORT
#include "../../bdsx/nodegate.h"

namespace nodegate {
NodeGateConfig* config;

} // namespace nodegate

void NODEGATE_EXPORT_ nodegate::setMainCallback(
    NodeGateConfig* _config) noexcept {
  config = _config;
}

namespace node {
namespace native_module {

using v8::Array;
using v8::ArrayBuffer;
using v8::ArrayBufferCreationMode;
using v8::Context;
using v8::EscapableHandleScope;
using v8::Function;
using v8::FunctionCallbackInfo;
using v8::HandleScope;
using v8::Integer;
using v8::IntegrityLevel;
using v8::Isolate;
using v8::Local;
using v8::Maybe;
using v8::MaybeLocal;
using v8::Object;
using v8::Script;
using v8::ScriptCompiler;
using v8::ScriptOrigin;
using v8::Set;
using v8::String;
using v8::Uint8Array;
using v8::Value;

// TODO(joyeecheung): make these more general and put them into util.h
Local<Object> MapToObject(Local<Context> context,
                          const NativeModuleRecordMap& in) {
  Isolate* isolate = context->GetIsolate();
  Local<Object> out = Object::New(isolate);
  for (auto const& x : in) {
    Local<String> key = OneByteString(isolate, x.first.c_str(), x.first.size());
    out->Set(context, key, x.second.ToStringChecked(isolate)).FromJust();
  }
  return out;
}

Local<Set> ToJsSet(Local<Context> context,
                   const std::set<std::string>& in) {
  Isolate* isolate = context->GetIsolate();
  Local<Set> out = Set::New(isolate);
  for (auto const& x : in) {
    out->Add(context, OneByteString(isolate, x.c_str(), x.size()))
        .ToLocalChecked();
  }
  return out;
}

void NativeModuleLoader::GetCacheUsage(
    const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  Isolate* isolate = env->isolate();
  Local<Context> context = env->context();
  Local<Object> result = Object::New(isolate);
  result
      ->Set(env->context(),
            OneByteString(isolate, "compiledWithCache"),
            ToJsSet(context, env->native_modules_with_cache))
      .FromJust();
  result
      ->Set(env->context(),
            OneByteString(isolate, "compiledWithoutCache"),
            ToJsSet(context, env->native_modules_without_cache))
      .FromJust();
  args.GetReturnValue().Set(result);
}

void NativeModuleLoader::GetSourceObject(
    const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  args.GetReturnValue().Set(per_process_loader.GetSourceObject(env->context()));
}

Local<Object> NativeModuleLoader::GetSourceObject(
    Local<Context> context) const {
  return MapToObject(context, source_);
}

Local<String> NativeModuleLoader::GetSource(Isolate* isolate,
                                            const char* id) const {
  const auto it = source_.find(id);
  CHECK_NE(it, source_.end());
  return it->second.ToStringChecked(isolate);
}

NativeModuleLoader::NativeModuleLoader() {
  LoadJavaScriptSource();
  LoadJavaScriptHash();
  LoadCodeCache();
  LoadCodeCacheHash();
}

void NativeModuleLoader::CompileCodeCache(
    const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(args[0]->IsString());
  node::Utf8Value id(env->isolate(), args[0].As<String>());

  // TODO(joyeecheung): allow compiling cache for bootstrapper by
  // switching on id
  MaybeLocal<Value> result =
      CompileAsModule(env, *id, CompilationResultType::kCodeCache);
  if (!result.IsEmpty()) {
    args.GetReturnValue().Set(result.ToLocalChecked());
  }
}

void NativeModuleLoader::CompileFunction(
    const FunctionCallbackInfo<Value>& args) {
  Environment* env = Environment::GetCurrent(args);
  CHECK(args[0]->IsString());
  node::Utf8Value id(env->isolate(), args[0].As<String>());

  MaybeLocal<Value> result =
      CompileAsModule(env, *id, CompilationResultType::kFunction);
  if (!result.IsEmpty()) {
    args.GetReturnValue().Set(result.ToLocalChecked());
  }
}

class JsCallImpl : public nodegate::JsCall {
 private:
  v8::Isolate* isolate;
  v8::Persistent<Context> context;

  class JsFunction {
   public:
    v8::Persistent<Function> func;
    
    void call(JsCallImpl* call) {
      v8::HandleScope _scope(call->isolate);

      Local<Context> context = call->context.Get(call->isolate);
      Local<Function> require = func.Get(call->isolate);
      require->Call(context, context->Global(), 0, nullptr);
    }
    void call(JsCallImpl* call, nodegate::StringView path) {
      v8::HandleScope _scope(call->isolate);

      v8::Handle<Value> v8path;
      v8::String::NewFromTwoByte(call->isolate,
                                 (const uint16_t*)path.string,
                                 v8::NewStringType::kNormal,
                                 path.length)
          .ToLocal(&v8path);
      Local<Context> context = call->context.Get(call->isolate);
      Local<Function> require = func.Get(call->isolate);
      require->Call(context, context->Global(), 1, &v8path);
    }
  };

 public:
  JsFunction m_callmain;
  JsFunction m_require;
  JsFunction m_log;
  JsFunction m_error;
  JsFunction m_tickCallback;

  JsCallImpl(v8::Isolate* isolate, v8::Handle<Context> context) noexcept
      : isolate(isolate), context(isolate, context) {}

  // Inherited via JsCall
  virtual void callMain() noexcept override {
    m_callmain.call(this);
  }
  virtual void require(nodegate::StringView path) noexcept override {
    m_require.call(this, path);
  }
  virtual void log(nodegate::StringView msg) noexcept override {
    m_log.call(this, msg);
  }
  virtual void error(nodegate::StringView msg) noexcept override {
    m_error.call(this, msg);
  }
  virtual void tickCallback() noexcept override {
    m_tickCallback.call(this);
  }
};


void NativeModuleLoader::_nodegate(
    const FunctionCallbackInfo<Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::Local<Context> context = isolate->GetCurrentContext();

  JsCallImpl* callImpl = new JsCallImpl(isolate, context);
  callImpl->m_callmain.func.Reset(isolate, args[0].As<Function>());
  callImpl->m_require.func.Reset(isolate, args[1].As<Function>());
  callImpl->m_log.func.Reset(isolate, args[2].As<Function>());
  callImpl->m_error.func.Reset(isolate, args[3].As<Function>());
  callImpl->m_tickCallback.func.Reset(isolate, args[4].As<Function>());
  nodegate::config->main_call(callImpl);
}
void NativeModuleLoader::_nodegate_stdout(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::Local<Context> context = isolate->GetCurrentContext();
  v8::Handle<Value> data = args[0];
  if (data->IsArrayBufferView()) {
    ArrayBuffer::Contents contents = args[0].As<v8::ArrayBufferView>()->Buffer()->GetContents();
    nodegate::config->stdout_call((const char*)contents.Data(), contents.ByteLength());
  } else {
    v8::String::Utf8Value utf8data(data->ToString());
    nodegate::config->stdout_call(*utf8data, utf8data.length());
  }
}
// stderr write hook
void NativeModuleLoader::_nodegate_stderr(
    const v8::FunctionCallbackInfo<v8::Value>& args) {
  v8::Isolate* isolate = args.GetIsolate();
  v8::Local<Context> context = isolate->GetCurrentContext();
  v8::Handle<Value> data = args[0];
  if (data->IsArrayBufferView()) {
    ArrayBuffer::Contents contents =
        args[0].As<v8::ArrayBufferView>()->Buffer()->GetContents();
    nodegate::config->stderr_call((const char*)contents.Data(),
                                  contents.ByteLength());
  } else {
    v8::String::Utf8Value utf8data(data->ToString());
    nodegate::config->stderr_call(*utf8data, utf8data.length());
  }
}
// TODO(joyeecheung): it should be possible to generate the argument names
// from some special comments for the bootstrapper case.
MaybeLocal<Value> NativeModuleLoader::CompileAndCall(
    Local<Context> context,
    const char* id,
    std::vector<Local<String>>* parameters,
    std::vector<Local<Value>>* arguments,
    Environment* optional_env) {
  Isolate* isolate = context->GetIsolate();
  MaybeLocal<Value> compiled = per_process_loader.LookupAndCompile(
      context, id, parameters, CompilationResultType::kFunction, nullptr);
  if (compiled.IsEmpty()) {
    return compiled;
  }
  Local<Function> fn = compiled.ToLocalChecked().As<Function>();
  return fn->Call(
      context, v8::Null(isolate), arguments->size(), arguments->data());
}

MaybeLocal<Value> NativeModuleLoader::CompileAsModule(
    Environment* env, const char* id, CompilationResultType result) {
  std::vector<Local<String>> parameters = {env->exports_string(),
                                           env->require_string(),
                                           env->module_string(),
                                           env->process_string(),
                                           env->internal_binding_string()};
  return per_process_loader.LookupAndCompile(
      env->context(), id, &parameters, result, env);
}

// Currently V8 only checks that the length of the source code is the
// same as the code used to generate the hash, so we add an additional
// check here:
// 1. During compile time, when generating node_javascript.cc and
//    node_code_cache.cc, we compute and include the hash of the
//    JavaScript source in both.
// 2. At runtime, we check that the hash of the code being compiled
//   and the hash of the code used to generate the cache
//   (without the parameters) is the same.
// This is based on the assumptions:
// 1. `code_cache_hash` must be in sync with `code_cache`
//     (both defined in node_code_cache.cc)
// 2. `source_hash` must be in sync with `source`
//     (both defined in node_javascript.cc)
// 3. If `source_hash` is in sync with `code_cache_hash`,
//    then the source code used to generate `code_cache`
//    should be in sync with the source code in `source`
// The only variable left, then, are the parameters passed to the
// CompileFunctionInContext. If the parameters used generate the cache
// is different from the one used to compile modules at run time, then
// there could be false postivies, but that should be rare and should fail
// early in the bootstrap process so it should be easy to detect and fix.

// Returns nullptr if there is no code cache corresponding to the id
ScriptCompiler::CachedData* NativeModuleLoader::GetCachedData(
    const char* id) const {
  const auto it = per_process_loader.code_cache_.find(id);
  // This could be false if the module cannot be cached somehow.
  // See lib/internal/bootstrap/cache.js on the modules that cannot be cached
  if (it == per_process_loader.code_cache_.end()) {
    return nullptr;
  }

  const uint8_t* code_cache_value = it->second.one_bytes_data();
  size_t code_cache_length = it->second.length();

  const auto it2 = code_cache_hash_.find(id);
  CHECK_NE(it2, code_cache_hash_.end());
  const std::string& code_cache_hash_value = it2->second;

  const auto it3 = source_hash_.find(id);
  CHECK_NE(it3, source_hash_.end());
  const std::string& source_hash_value = it3->second;

  // It may fail when any of the inputs of the `node_js2c` target in
  // node.gyp is modified but the tools/generate_code_cache.js
  // is not re run.
  // FIXME(joyeecheung): Figure out how to resolve the dependency issue.
  // When the code cache was introduced we were at a point where refactoring
  // node.gyp may not be worth the effort.
  CHECK_EQ(code_cache_hash_value, source_hash_value);

  return new ScriptCompiler::CachedData(code_cache_value, code_cache_length);
}

// Returns Local<Function> of the compiled module if return_code_cache
// is false (we are only compiling the function).
// Otherwise return a Local<Object> containing the cache.
MaybeLocal<Value> NativeModuleLoader::LookupAndCompile(
    Local<Context> context,
    const char* id,
    std::vector<Local<String>>* parameters,
    CompilationResultType result_type,
    Environment* optional_env) {
  Isolate* isolate = context->GetIsolate();
  EscapableHandleScope scope(isolate);
  Local<Value> ret;  // Used to convert to MaybeLocal before return

  Local<String> source = GetSource(isolate, id);

  std::string filename_s = id + std::string(".js");
  Local<String> filename =
      OneByteString(isolate, filename_s.c_str(), filename_s.size());
  Local<Integer> line_offset = Integer::New(isolate, 0);
  Local<Integer> column_offset = Integer::New(isolate, 0);
  ScriptOrigin origin(filename, line_offset, column_offset);

  bool use_cache = false;
  ScriptCompiler::CachedData* cached_data = nullptr;

  // 1. We won't even check the existence of the cache if the binary is not
  //    built with them.
  // 2. If we are generating code cache for tools/general_code_cache.js, we
  //    are not going to use any cache ourselves.
  if (has_code_cache_ && result_type == CompilationResultType::kFunction) {
    cached_data = GetCachedData(id);
    if (cached_data != nullptr) {
      use_cache = true;
    }
  }

  ScriptCompiler::Source script_source(source, origin, cached_data);

  ScriptCompiler::CompileOptions options;
  if (result_type == CompilationResultType::kCodeCache) {
    options = ScriptCompiler::kEagerCompile;
  } else if (use_cache) {
    options = ScriptCompiler::kConsumeCodeCache;
  } else {
    options = ScriptCompiler::kNoCompileOptions;
  }

  MaybeLocal<Function> maybe_fun =
      ScriptCompiler::CompileFunctionInContext(context,
                                               &script_source,
                                               parameters->size(),
                                               parameters->data(),
                                               0,
                                               nullptr,
                                               options);

  // This could fail when there are early errors in the native modules,
  // e.g. the syntax errors
  if (maybe_fun.IsEmpty()) {
    // In the case of early errors, v8 is already capable of
    // decorating the stack for us - note that we use CompileFunctionInContext
    // so there is no need to worry about wrappers.
    return MaybeLocal<Value>();
  }

  Local<Function> fun = maybe_fun.ToLocalChecked();
  if (use_cache) {
    if (optional_env != nullptr) {
      // This could happen when Node is run with any v8 flag, but
      // the cache is not generated with one
      if (script_source.GetCachedData()->rejected) {
        optional_env->native_modules_without_cache.insert(id);
      } else {
        optional_env->native_modules_with_cache.insert(id);
      }
    }
  } else {
    if (optional_env != nullptr) {
      optional_env->native_modules_without_cache.insert(id);
    }
  }

  if (result_type == CompilationResultType::kCodeCache) {
    std::unique_ptr<ScriptCompiler::CachedData> cached_data(
        ScriptCompiler::CreateCodeCacheForFunction(fun));
    CHECK_NE(cached_data, nullptr);
    size_t cached_data_length = cached_data->length;
    // Since we have no special allocator to create an ArrayBuffer
    // from a new'ed pointer, we will need to copy it - but this
    // code path is only run by the tooling that generates the code
    // cache to be bundled in the binary
    // so it should be fine.
    MallocedBuffer<uint8_t> copied(cached_data->length);
    memcpy(copied.data, cached_data->data, cached_data_length);
    Local<ArrayBuffer> buf =
        ArrayBuffer::New(isolate,
                         copied.release(),
                         cached_data_length,
                         ArrayBufferCreationMode::kInternalized);
    ret = Uint8Array::New(buf, 0, cached_data_length);
  } else {
    ret = fun;
  }

  return scope.Escape(ret);
}

void NativeModuleLoader::Initialize(Local<Object> target,
                                    Local<Value> unused,
                                    Local<Context> context,
                                    void* priv) {
  Environment* env = Environment::GetCurrent(context);

  env->SetMethod(
      target, "getSource", NativeModuleLoader::GetSourceObject);
  env->SetMethod(
      target, "getCacheUsage", NativeModuleLoader::GetCacheUsage);
  env->SetMethod(
      target, "compileFunction", NativeModuleLoader::CompileFunction);
  env->SetMethod(
      target, "compileCodeCache", NativeModuleLoader::CompileCodeCache);
  if (nodegate::config) {
    env->SetMethod(target, "_nodegate", NativeModuleLoader::_nodegate);
    env->SetMethod(target, "_nodegate_stdout", NativeModuleLoader::_nodegate_stdout);
    env->SetMethod(target, "_nodegate_stderr", NativeModuleLoader::_nodegate_stderr);
  }
  // internalBinding('native_module') should be frozen
  target->SetIntegrityLevel(context, IntegrityLevel::kFrozen).FromJust();
}

}  // namespace native_module
}  // namespace node

NODE_MODULE_CONTEXT_AWARE_INTERNAL(
    native_module, node::native_module::NativeModuleLoader::Initialize)
