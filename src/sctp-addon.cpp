#include <nan.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <netinet/sctp.h>
#include <arpa/inet.h>
#include <signal.h>

using v8::FunctionCallbackInfo;
using v8::FunctionTemplate;
using v8::Persistent;
using v8::Context;
using v8::Isolate;
using v8::Function;
using v8::Local;
using v8::Object;
using v8::String;
using v8::Value;
using v8::Array;
using v8::ArrayBuffer;

int debug = 0;
int pendingReceived = 0;
int pendingSent = 0;
int sock;

struct tm * getCurrentTime() {
    time_t t = time(0);
    struct tm *now = localtime(&t);

    return now;
}

void Debug(const Nan::FunctionCallbackInfo<v8::Value>& args) {

    debug = args[0]->NumberValue();
}

void Connect(const Nan::FunctionCallbackInfo<v8::Value>& args) {

    Isolate* isolate = args.GetIsolate();
    Local<Value> error[2];
    int result;
    struct sockaddr_in *remoteAddrs, *addrs, addr;
    struct sctp_initmsg initmsg;
    struct timeval timeout;
    sctp_assoc_t id;
    fd_set fdset;

    errno = 0;

    if (!args[0]->IsObject()) {
        Nan::ThrowTypeError("Raw socket must be an object");
        return;
    }

    if (!args[1]->IsArray()) {
        Nan::ThrowTypeError("Hosts must be an array");
        return;
    }

    if (!args[2]->IsArray()) {
        Nan::ThrowTypeError("Remote hosts must be an array");
        return;
    }

    if (!args[3]->IsNumber()) {
        Nan::ThrowTypeError("Port must be a number");
        return;
    }

    if (!args[4]->IsObject()) {
        Nan::ThrowTypeError("Init message options must an object");
        return;
    }

    Local<Object> rawSocket = args[0]->ToObject();
    Local<Array> hosts = Local<Array>::Cast(args[1]);
    Local<Array> remoteHosts = Local<Array>::Cast(args[2]);
    int port = args[3]->NumberValue();
    Local<Object> initOpts = args[4]->ToObject();
    Local<Function> cb = Local<Function>::Cast(args[5]);

    sock = socket (PF_INET, SOCK_SEQPACKET|SOCK_NONBLOCK, IPPROTO_SCTP);

    if (sock == -1) {
        if (debug) {
            printf("Unable to create socket. Error code: %d\n", errno);
        }
        error[0] = String::NewFromUtf8(isolate, "Unable to create socket.");
        cb->Function::Call(Null(isolate), 1, error);
        return;
    }

    addrs = (struct sockaddr_in *) malloc(sizeof(struct sockaddr_in) * hosts->Array::Length());
    memset(&addr, 0, sizeof(struct sockaddr_in));

    for (int i = 0; i < (int)hosts->Array::Length(); i++) {

        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(*String::Utf8Value(hosts->Get(i)->ToString()));

        memcpy(addrs + i, &addr, sizeof(struct sockaddr_in));
    }

    result = sctp_bindx(sock, (struct sockaddr *)addrs, hosts->Array::Length(), SCTP_BINDX_ADD_ADDR);

    if (result == -1)
    {
        if (debug) {
            printf("Cannot bind socket to specified addresses. Error code: %d\n", errno);
        }
        error[0] = String::NewFromUtf8(isolate, "Cannot bind socket to specified addresses.");
        cb->Function::Call(Null(isolate), 1, error);
        return;
    }

    memset (&initmsg, 0, sizeof (initmsg));
    Local<Array> props = initOpts->GetOwnPropertyNames();

    for (int i = 0; i < (int)props->Array::Length(); i++) {
        Local<Value> key = props->Get(i);
        if (strcmp(*String::Utf8Value(key), "ostreams") == 0) {
            initmsg.sinit_num_ostreams = initOpts->Get(key)->NumberValue();
        }
        else if (strcmp(*String::Utf8Value(key), "maxInstreams") == 0) {
            initmsg.sinit_max_instreams = initOpts->Get(key)->NumberValue();
        }
        else if (strcmp(*String::Utf8Value(key), "maxAttempts") == 0) {
            initmsg.sinit_max_attempts = initOpts->Get(key)->NumberValue();
        }
        else if (strcmp(*String::Utf8Value(key), "timeout") == 0) {
            timeout.tv_sec = initOpts->Get(key)->NumberValue();
        }
    }

    result = setsockopt (sock, IPPROTO_SCTP, SCTP_INITMSG, &initmsg, sizeof (initmsg));

    if (result == -1)
    {
        if (debug) {
            printf("Cannot set init options to socket. Error code: %d\n", errno);
        }
        error[0] = String::NewFromUtf8(isolate, "Cannot set init options to socket.");
        cb->Function::Call(Null(isolate), 1, error);
    }

    remoteAddrs = (struct sockaddr_in *) malloc(sizeof(struct sockaddr_in) * remoteHosts->Array::Length());

    for (int i = 0; i < (int)remoteHosts->Array::Length(); i++) {

        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(*String::Utf8Value(remoteHosts->Get(i)->ToString()));

        memcpy(remoteAddrs + i, &addr, sizeof(struct sockaddr_in));
    }

    FD_ZERO(&fdset);
    FD_SET(sock, &fdset);

    result = sctp_connectx(sock, (struct sockaddr *)remoteAddrs, remoteHosts->Array::Length(), &id);

    if (errno != EINPROGRESS) {
        if (debug) {
            printf("Cannot connect to specified addresses. Error code: %d\n", errno);
        }
        error[0] = String::NewFromUtf8(isolate, "Cannot connect to specified addresses.");
        cb->Function::Call(Null(isolate), 1, error);
        return;
    }
    else if (result == -1 && errno == EINPROGRESS) {
        if (debug) {
            printf("Trying to connect with specified addresses on specified port.\n");
        }

        int so_error;
        socklen_t opt_len = (socklen_t)sizeof(int);

        select(sock + 1, 0, &fdset, 0, &timeout);

        if (FD_ISSET(sock, &fdset)) {
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &opt_len) != 0) {
                if (debug) {
                    printf("Getsockopt error on connect. Error code: %d\n", errno);
                }
                error[0] = String::NewFromUtf8(isolate, "Cannot connect to specified addresses.");
                cb->Function::Call(Null(isolate), 1, error);
                return;
            }
            if (so_error) {
                if (debug) {
                    printf("Connection failed. Error code: %d\n", errno);
                }
                error[0] = String::NewFromUtf8(isolate, "Cannot connect to specified addresses.");
                cb->Function::Call(Null(isolate), 1, error);
                return;
            }
            if (debug) {
                printf("Connection successful. Association ID: %d\n", id);
            }

            char idBuffer[10];
            char response[4096];

            snprintf(idBuffer, sizeof(idBuffer), "%d", id);
            rawSocket->Object::Set(String::NewFromUtf8(isolate, "assocId"), String::NewFromUtf8(isolate, idBuffer));
            error[0] = String::NewFromUtf8(isolate, "");
            error[1] = rawSocket;

            cb->Function::Call(Null(isolate), 2, error);

            while(1) {
                result = sctp_recvmsg(sock, (void *)&response, (size_t)sizeof(response), NULL, 0, 0, 0);
                if (result > 0 && result < 4095) {
                    if (debug) {
                        struct tm *now = getCurrentTime();
                        printf("%i/%i/%i %i:%i:%i ", now->tm_year + 1900, now->tm_mon + 1, now->tm_mday, now->tm_hour, now->tm_min, now->tm_sec);
                        printf("Server replied (size %d)\n", result);
                    }
                    Local<ArrayBuffer> buffer = ArrayBuffer::New(isolate, (void *)&response, (size_t)result);
                    Local<Value> message[2] = { String::NewFromUtf8(isolate, "data"),  buffer};
                    Local<Function> emit = Local<Function>::Cast(rawSocket->Object::Get(String::NewFromUtf8(isolate, "emit")));
                    emit->Function::Call(rawSocket, 2, message);
                    result = 0;
                }
            }
        } else {
            if (debug) {
                printf("Connection attempt timed out. Error code: %d\n", errno);
            }
            error[0] = String::NewFromUtf8(isolate, "Connection attempt timed out.");
            cb->Function::Call(Null(isolate), 1, error);
            return;
        }
    }
}

void Send(const Nan::FunctionCallbackInfo<v8::Value>& args) {
    Isolate* isolate = args.GetIsolate();
    Local<Value> error[2];
    Local<Object> bufferObj = args[0]->ToObject();

    char *bufferData = node::Buffer::Data(bufferObj);
    size_t bufferLength = node::Buffer::Length(bufferObj);
    int result;

    result = sctp_sendmsg (sock, (void *) bufferData, bufferLength, NULL, 0, 0, 0, 0, 0, 0);

    if (result == -1) {
        if (debug) {
            printf("Cannot send message. Error code: %d\n", errno);
        }
        return;
    }
    if (debug) {
        struct tm *now = getCurrentTime();
        printf("%i/%i/%i %i:%i:%i ", now->tm_year + 1900, now->tm_mon + 1, now->tm_mday, now->tm_hour, now->tm_min, now->tm_sec);
        printf("Message successfully sent (%d bytes).\n", result);
    }
}

void Init(v8::Local<v8::Object> exports) {
    exports->Set(Nan::New("connect").ToLocalChecked(),
                Nan::New<v8::FunctionTemplate>(Connect)->GetFunction());
    exports->Set(Nan::New("debug").ToLocalChecked(),
                Nan::New<v8::FunctionTemplate>(Debug)->GetFunction());
    exports->Set(Nan::New("send").ToLocalChecked(),
                Nan::New<v8::FunctionTemplate>(Send)->GetFunction());
}

NODE_MODULE(exports, Init)
