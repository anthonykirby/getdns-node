/*
 * Copyright (c) 2014, Verisign, Inc.
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions are met:
 * * Redistributions of source code must retain the above copyright
 *   notice, this list of conditions and the following disclaimer.
 * * Redistributions in binary form must reproduce the above copyright
 *   notice, this list of conditions and the following disclaimer in the
 *   documentation and/or other materials provided with the distribution.
 * * Neither the names of the copyright holders nor the
 *   names of its contributors may be used to endorse or promote products
 *   derived from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL Verisign, Inc. BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 * SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include "GNContext.h"
#include "GNUtil.h"
#include "GNConstants.h"

#include <getdns/getdns_extra.h>
#include <arpa/inet.h>
#include <node_buffer.h>
#include <string.h>
#include <nan.h>
#include <sys/time.h>

using namespace v8;

// Enum to distinguish which helper is being used.
typedef enum LookupType {
    GNAddress = 0,
    GNHostname,
    GNService
} LookupType;

// Callback data passed to getdns callback as userarg
typedef struct CallbackData {
    NanCallback* callback;
    GNContext* ctx;
} CallbackData;

// Helper to create an error object for lookup callbacks
static Handle<Value> makeErrorObj(const char* msg, int code) {
    Handle<Object> obj = NanNew<Object>();
    obj->Set(NanNew<String>("msg"), NanNew<String>(msg));
    obj->Set(NanNew<String>("code"), NanNew<Integer>(code));
    return obj;
}

// Helper to create an address dictionary from string
// Must be freed by the user
static getdns_dict* getdns_util_create_ip(const char* ip) {

    getdns_return_t r;
    const char* ipType;
    getdns_bindata ipData;
    uint8_t addrBuff[16];
    size_t addrSize = 0;
    getdns_dict* result = NULL;

    if (!ip) {
        return NULL;
    }
    // convert to bytes
    if (inet_pton(AF_INET, ip, &addrBuff) == 1) {
        addrSize = 4;
    } else if (inet_pton(AF_INET6, ip, &addrBuff) == 1) {
        addrSize = 16;
    }
    if (addrSize == 0) {
        return NULL;
    }
    // create the dict
    result = getdns_dict_create();
    if (!result) {
        return NULL;
    }
    // set fields
    ipType = addrSize == 4 ? "IPv4" : "IPv6";
    r = getdns_dict_util_set_string(result, (char*) "address_type", ipType);
    if (r != GETDNS_RETURN_GOOD) {
        getdns_dict_destroy(result);
        return NULL;
    }
    ipData.data = addrBuff;
    ipData.size = addrSize;
    r = getdns_dict_set_bindata(result, "address_data", &ipData);
    if (r != GETDNS_RETURN_GOOD) {
        getdns_dict_destroy(result);
        return NULL;
    }
    return result;
}

// Setter functions
typedef getdns_return_t (*getdns_context_uint8_t_setter)(getdns_context*, uint8_t);
typedef getdns_return_t (*getdns_context_uint16_t_setter)(getdns_context*, uint16_t);

static void setTransport(getdns_context* context, Handle<Value> opt) {
    if (opt->IsNumber()) {
        uint32_t num = opt->Uint32Value();
        getdns_context_set_dns_transport(context, (getdns_transport_t) num);
    }
}

static void setStub(getdns_context* context, Handle<Value> opt) {
    if (opt->IsTrue()) {
        getdns_context_set_resolution_type(context, GETDNS_RESOLUTION_STUB);
    } else {
        getdns_context_set_resolution_type(context, GETDNS_RESOLUTION_RECURSING);
    }
}

static void setResolutionType(getdns_context* context, Handle<Value> opt) {
    if (opt->IsNumber()) {
        uint32_t num = opt->Uint32Value();
        getdns_context_set_resolution_type(context, (getdns_resolution_t) num);
    }
}

static void setUpstreams(getdns_context* context, Handle<Value> opt) {
    if (opt->IsArray()) {
        getdns_list* upstreams = getdns_list_create();
        Handle<Array> values = Handle<Array>::Cast(opt);
        for (uint32_t i = 0; i < values->Length(); ++i) {
            Local<Value> ipOrTuple = values->Get(i);
            getdns_dict* ipDict = NULL;
            if (ipOrTuple->IsArray()) {
                // two tuple - first is IP, 2nd is port
                Handle<Array> tuple = Handle<Array>::Cast(ipOrTuple);
                if (tuple->Length() > 0) {
                    NanUtf8String asciiStr(tuple->Get(0)->ToString());
                    ipDict = getdns_util_create_ip(*asciiStr);
                    if (ipDict && tuple->Length() > 1 &&
                        tuple->Get(1)->IsNumber()) {
                        // port
                        uint32_t port = tuple->Get(1)->Uint32Value();
                        getdns_dict_set_int(ipDict, "port", port);
                    }
                }
            } else {
                NanUtf8String asciiStr(ipOrTuple->ToString());
                ipDict = getdns_util_create_ip(*asciiStr);
            }
            if (ipDict) {
                size_t len = 0;
                getdns_list_get_length(upstreams, &len);
                getdns_list_set_dict(upstreams, len, ipDict);
                getdns_dict_destroy(ipDict);
            } else {
                NanUtf8String msg(String::Concat(NanNew<String>("Upstream value is invalid: "), ipOrTuple->ToString()));
                NanThrowTypeError(*msg);
            }
        }
        getdns_return_t r = getdns_context_set_upstream_recursive_servers(context, upstreams);
        getdns_list_destroy(upstreams);
        if (r != GETDNS_RETURN_GOOD) {
            NanThrowTypeError("Failed to set upstreams.");
        }
    }
}

static void setTimeout(getdns_context* context, Handle<Value> opt) {
    if (opt->IsNumber()) {
        uint32_t num = opt->Uint32Value();
        getdns_context_set_timeout(context, num);
    }
}

static void setUseThreads(getdns_context* context, Handle<Value> opt) {
    int val = opt->IsTrue() ? 1 : 0;
    getdns_context_set_use_threads(context, val);
}

static void setReturnDnssecStatus(getdns_context* context, Handle<Value> opt) {
    int val = opt->IsTrue() ? GETDNS_EXTENSION_TRUE : GETDNS_EXTENSION_FALSE;
    getdns_context_set_return_dnssec_status(context, val);
}

typedef void (*context_setter)(getdns_context* context, Handle<Value> opt);
typedef struct OptionSetter {
    const char* opt_name;
    context_setter setter;
} OptionSetter;

static OptionSetter SETTERS[] = {
    { "stub", setStub },
    { "upstreams", setUpstreams },
    { "upstream_recursive_servers", setUpstreams },
    { "timeout", setTimeout },
    { "use_threads", setUseThreads },
    { "return_dnssec_status", setReturnDnssecStatus },
    { "dns_transport", setTransport},
    { "resolution_type", setResolutionType }
};

static size_t NUM_SETTERS = sizeof(SETTERS) / sizeof(OptionSetter);

typedef struct Uint8OptionSetter {
    const char* opt_name;
    getdns_context_uint8_t_setter setter;
} Uint8OptionSetter;

static Uint8OptionSetter UINT8_OPTION_SETTERS[] = {
    { "edns_extended_rcode", getdns_context_set_edns_extended_rcode },
    { "edns_version", getdns_context_set_edns_version },
    { "edns_do_bit", getdns_context_set_edns_do_bit }
};

static size_t NUM_UINT8_SETTERS = sizeof(UINT8_OPTION_SETTERS) / sizeof(Uint8OptionSetter);

typedef struct Uint16OptionSetter {
    const char* opt_name;
    getdns_context_uint16_t_setter setter;
} Uint16OptionSetter;

static Uint16OptionSetter UINT16_OPTION_SETTERS[] = {
    { "limit_outstanding_queries", getdns_context_set_limit_outstanding_queries },
    { "edns_maximum_udp_payloadSize", getdns_context_set_edns_maximum_udp_payload_size }
};

static size_t NUM_UINT16_SETTERS = sizeof(UINT16_OPTION_SETTERS) / sizeof(Uint16OptionSetter);

// End setters
NAN_GETTER(GNContext::GetContextValue) {
    // context has no getters yet
    NanScope();
    NanReturnValue(NanNew<Integer>(-1));
}
NAN_SETTER(GNContext::SetContextValue) {
    // walk setters
    NanUtf8String name(property);
    GNContext* ctx = node::ObjectWrap::Unwrap<GNContext>(args.This());
    if (!ctx) {
        NanThrowError("Context is invalid.");
    }
    size_t s = 0;
    bool found = false;
    for (s = 0; s < NUM_SETTERS && !found; ++s) {
        if (strcmp(SETTERS[s].opt_name, *name) == 0) {
            SETTERS[s].setter(ctx->context_, value);
            found = true;
            break;
        }
    }
    if (!value->IsNumber()) {
        return;
    }
    for (s = 0; s < NUM_UINT8_SETTERS && !found; ++s) {
        if (strcmp(UINT8_OPTION_SETTERS[s].opt_name, *name) == 0) {
            found = true;
            uint32_t optVal = value->Uint32Value();
            UINT8_OPTION_SETTERS[s].setter(ctx->context_, (uint8_t)optVal);
        }
    }
    for (s = 0; s < NUM_UINT16_SETTERS && !found; ++s) {
        if (strcmp(UINT16_OPTION_SETTERS[s].opt_name, *name) == 0) {
            found = true;
            uint32_t optVal = value->Uint32Value();
            UINT16_OPTION_SETTERS[s].setter(ctx->context_, (uint16_t)optVal);
        }
    }
}

void GNContext::InitProperties(Handle<Object> ctx) {
    size_t s = 0;
    for (s = 0; s < NUM_SETTERS; ++s) {
        ctx->SetAccessor(NanNew<String>(SETTERS[s].opt_name),
            GNContext::GetContextValue, GNContext::SetContextValue);
    }
    for (s = 0; s < NUM_UINT8_SETTERS; ++s) {
        ctx->SetAccessor(NanNew<String>(UINT8_OPTION_SETTERS[s].opt_name),
            GNContext::GetContextValue, GNContext::SetContextValue);
    }
    for (s = 0; s < NUM_UINT16_SETTERS; ++s) {
        ctx->SetAccessor(NanNew<String>(UINT16_OPTION_SETTERS[s].opt_name),
            GNContext::GetContextValue, GNContext::SetContextValue);

    }
}

GNContext::GNContext() : context_(NULL) { }
GNContext::~GNContext() {
    getdns_context_destroy(context_);
    context_ = NULL;
}

void GNContext::ApplyOptions(Handle<Object> self, Handle<Value> optsV) {
    if (!GNUtil::isDictionaryObject(optsV)) {
        return;
    }
    TryCatch try_catch;
    Local<Object> opts = optsV->ToObject();
    Local<Array> names = opts->GetOwnPropertyNames();
    // walk properties
    for(unsigned int i = 0; i < names->Length(); i++) {
        Local<Value> nameVal = names->Get(i);
        Local<Value> opt = opts->Get(nameVal);
        self->Set(nameVal, opt);
        if (try_catch.HasCaught()) {
            try_catch.ReThrow();
            return;
        }
    }
}

// Module initialization
void GNContext::Init(Handle<Object> target) {
    // prepare context object template
    Local<FunctionTemplate> jsContextTpl = NanNew<FunctionTemplate>(GNContext::New);
    jsContextTpl->SetClassName(NanNew<String>("Context"));
    jsContextTpl->InstanceTemplate()->SetInternalFieldCount(1);
    // Prototype
    NODE_SET_PROTOTYPE_METHOD(jsContextTpl, "lookup", GNContext::Lookup);
    NODE_SET_PROTOTYPE_METHOD(jsContextTpl, "cancel", GNContext::Cancel);
    NODE_SET_PROTOTYPE_METHOD(jsContextTpl, "destroy", GNContext::Destroy);
    // Helpers - delegate to the same function w/ different data
    jsContextTpl->PrototypeTemplate()->Set(NanNew<String>("getAddress"),
        NanNew<FunctionTemplate>(GNContext::HelperLookup, NanNew<Integer>(GNAddress))->GetFunction());
    jsContextTpl->PrototypeTemplate()->Set(NanNew<String>("getHostname"),
        NanNew<FunctionTemplate>(GNContext::HelperLookup, NanNew<Integer>(GNHostname))->GetFunction());
    jsContextTpl->PrototypeTemplate()->Set(NanNew<String>("getService"),
        NanNew<FunctionTemplate>(GNContext::HelperLookup, NanNew<Integer>(GNService))->GetFunction());

    // Add the constructor
    target->Set(NanNew<String>("Context"), jsContextTpl->GetFunction());

    // Export constants
    GNConstants::Init(target);
}

// Explicity destroy the context
NAN_METHOD(GNContext::Destroy) {
    NanScope();
    GNContext* ctx = ObjectWrap::Unwrap<GNContext>(args.This());
    if (!ctx) {
        NanThrowError(NanNew<String>("Context is invalid."));
    }
    getdns_context_destroy(ctx->context_);
    ctx->context_ = NULL;
    NanReturnValue(NanTrue());
}

// Create a context (new op)
NAN_METHOD(GNContext::New) {
    NanScope();
    if (args.IsConstructCall()) {
        // new obj
        GNContext* ctx = new GNContext();
        getdns_return_t r = getdns_context_create(&ctx->context_, 1);
        if (r != GETDNS_RETURN_GOOD) {
            // Failed to create an underlying context
            delete ctx;
            NanThrowError(NanNew<String>("Unable to create GNContext."));
        }

        // Attach the context to node
        bool attached = GNUtil::attachContextToNode(ctx->context_);
        if (!attached) {
            // Bail
            delete ctx;
            NanThrowError(NanNew<String>("Unable to attach to Node."));
            NanReturnUndefined();
        }
        ctx->Wrap(args.This());
        // add setters
        GNContext::InitProperties(args.This());
        // Apply options if needed
        if (args.Length() > 0) {
            // could throw an
            TryCatch try_catch;
            GNContext::ApplyOptions(args.This(), args[0]);
            if (try_catch.HasCaught()) {
                // Need to bail
                delete ctx;
                try_catch.ReThrow();
                NanReturnUndefined();
            }
        }
        NanReturnThis();
    } else {
        NanThrowError(NanNew<String>("Must use new."));
    }
    NanReturnUndefined();
}

void GNContext::Callback(getdns_context *context,
                         getdns_callback_type_t cbType,
                         getdns_dict *response,
                         void *userArg,
                         getdns_transaction_t transId) {
    CallbackData* data = static_cast<CallbackData*>(userArg);
    // Setup the callback arguments
    Handle<Value> argv[3];
    if (cbType == GETDNS_CALLBACK_COMPLETE) {
        argv[0] = NanNull();
        argv[1] = GNUtil::convertToJSObj(response);
        getdns_dict_destroy(response);
    } else {
        argv[0] = makeErrorObj("Lookup failed.", cbType);
        argv[1] = NanNull();
    }
    TryCatch try_catch;
    argv[2] = GNUtil::convertToBuffer(&transId, 8);
    data->callback->Call(NanGetCurrentContext()->Global(), 3, argv);

    if (try_catch.HasCaught())
        node::FatalException(try_catch);

    // Unref
    data->ctx->Unref();
    delete data->callback;
    delete data;
}

// Cancel a req.  Expect it to be a transaction id as a buffer
NAN_METHOD(GNContext::Cancel) {
    NanScope();
    GNContext* ctx = node::ObjectWrap::Unwrap<GNContext>(args.This());
    if (!ctx || !ctx->context_) {
        NanReturnValue(NanFalse());
    }
    if (args.Length() < 1) {
        NanReturnValue(NanFalse());
    }
    if (node::Buffer::Length(args[0]) != 8) {
        NanReturnValue(NanFalse());
    }
    uint64_t transId;
    memcpy(&transId, node::Buffer::Data(args[0]), 8);
    getdns_return_t r = getdns_cancel_callback(ctx->context_, transId);
    NanReturnValue(r == GETDNS_RETURN_GOOD ? NanTrue() : NanFalse());
}

// Handle getdns general
NAN_METHOD(GNContext::Lookup) {
    NanScope();
    // name, type, and callback are required
    if (args.Length() < 3) {
        NanThrowTypeError("At least 3 arguments are required.");
    }
    // last arg must be a callback
    Local<Value> last = args[args.Length() - 1];
    if (!last->IsFunction()) {
        NanThrowTypeError("Final argument must be a function.");
    }
    Local<Function> localCb = Local<Function>::Cast(last);
    GNContext* ctx = node::ObjectWrap::Unwrap<GNContext>(args.This());
    if (!ctx || !ctx->context_) {
        Handle<Value> err = makeErrorObj("Context is invalid", GETDNS_RETURN_GENERIC_ERROR);
        Handle<Value> cbArgs[] = { err };
        NanMakeCallback(NanGetCurrentContext()->Global(), localCb, 1, cbArgs);
        NanReturnUndefined();
    }
    // take first arg and make it a string
    String::Utf8Value name(args[0]->ToString());
    // second arg must be a number
    if (!args[1]->IsNumber()) {
        Handle<Value> err = makeErrorObj("Second argument must be a number", GETDNS_RETURN_INVALID_PARAMETER);
        Handle<Value> cbArgs[] = { err };
        NanMakeCallback(NanGetCurrentContext()->Global(), localCb, 1, cbArgs);
        NanReturnUndefined();
    }
    uint16_t type = (uint16_t) args[1]->Uint32Value();

    // optional third arg is an object
    getdns_dict* extension = NULL;
    if (args.Length() > 3 && args[2]->IsObject()) {
        extension = GNUtil::convertToDict(args[2]->ToObject());
    }

    // create callback data
    CallbackData *data = new CallbackData();
    data->callback = new NanCallback(localCb);
    data->ctx = ctx;
    ctx->Ref();

    // issue a query
    getdns_transaction_t transId;
    getdns_return_t r = getdns_general(ctx->context_, *name, type,
                                       extension, data, &transId,
                                       GNContext::Callback);
    if (r != GETDNS_RETURN_GOOD) {
        // fail
        delete data->callback;
        data->ctx->Unref();
        delete data;

        Handle<Value> err = makeErrorObj("Error issuing query", r);
        Handle<Value> cbArgs[] = { err };
        localCb->Call(NanGetCurrentContext()->Global(), 1, cbArgs);
        NanReturnUndefined();
    }
    // done.
    NanReturnValue(GNUtil::convertToBuffer(&transId, 8));
}

// Common function to handle getdns_address/service/hostname
NAN_METHOD(GNContext::HelperLookup) {
    // first argument is a string
    // last argument must be a callback
    // optional argument of extensions
    NanScope();
    // name, type, and callback are required
    if (args.Length() < 2) {
        NanThrowTypeError("At least 2 arguments are required.");
    }
    // last arg must be a callback
    Local<Value> last = args[args.Length() - 1];
    if (!last->IsFunction()) {
        NanThrowTypeError("Final argument must be a function.");
    }
    Local<Function> localCb = Local<Function>::Cast(last);
    GNContext* ctx = node::ObjectWrap::Unwrap<GNContext>(args.This());
    if (!ctx || !ctx->context_) {
        Handle<Value> err = makeErrorObj("Context is invalid", GETDNS_RETURN_GENERIC_ERROR);
        Handle<Value> cbArgs[] = { err };
        localCb->Call(NanGetCurrentContext()->Global(), 1, cbArgs);
        NanReturnUndefined();
    }
    // take first arg and make it a string
    String::Utf8Value name(args[0]->ToString());

    // 2nd arg could be extensions
    // optional third arg is an object
    getdns_dict* extension = NULL;
    if (args.Length() > 2 && args[1]->IsObject()) {
        extension = GNUtil::convertToDict(args[1]->ToObject());
    }

    // figure out what called us
    uint32_t funcType = args.Data()->Uint32Value();
    // create callback data
    CallbackData *data = new CallbackData();
    data->callback = new NanCallback(localCb);
    data->ctx = ctx;
    ctx->Ref();

    getdns_transaction_t transId;
    getdns_return_t r = GETDNS_RETURN_GOOD;
    if (funcType == GNAddress) {
        r = getdns_address(ctx->context_, *name, extension,
                           data, &transId, GNContext::Callback);
    } else if(funcType == GNService) {
        r = getdns_service(ctx->context_, *name, extension,
                           data, &transId, GNContext::Callback);
    } else {
        // hostname
        // convert to a dictionary..
        getdns_dict* ip = getdns_util_create_ip(*name);
        if (ip) {
            r = getdns_hostname(ctx->context_, ip, extension,
                                data, &transId, GNContext::Callback);
            getdns_dict_destroy(ip);
        } else {
            r = GETDNS_RETURN_GENERIC_ERROR;
        }
    }

    if (r != GETDNS_RETURN_GOOD) {
        // fail
        delete data->callback;
        data->ctx->Unref();
        delete data;

        Handle<Value> err = makeErrorObj("Error issuing query", r);
        Handle<Value> cbArgs[] = { err };
        localCb->Call(NanGetCurrentContext()->Global(), 1, cbArgs);
        NanReturnUndefined();
    }
    // done. return as buffer
    NanReturnValue(GNUtil::convertToBuffer(&transId, 8));
}

// Init the module
NODE_MODULE(getdns, GNContext::Init)
