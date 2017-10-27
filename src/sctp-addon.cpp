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
#include <iostream>
#include <string>
#include <algorithm>
#include <iterator>
#include <thread>
#include <deque>
#include <mutex>
#include <chrono>
#include <condition_variable>

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
using namespace Nan;

int debug = 1;
int sock;

struct tm * getCurrentTime() {
    time_t t = time(0);
    struct tm *now = localtime(&t);

    return now;
}

class Message {
public:
  const char *buffer;
  size_t length;
  Message(const char *buffer, size_t length) : buffer(buffer), length(length){}
};

template<typename Data>
class WriteQueue
{
public:
    void write(Data data) {
        while (true) {
            std::unique_lock<std::mutex> locker(mu);
            buffer_.push_back(data);
            locker.unlock();
            cond.notify_one();
            return;
        }
    }
    Data pop() {
        while (true)
        {
            std::unique_lock<std::mutex> locker(mu);
            while (buffer_.empty())
            {
              cond.wait(locker);
            }
            Data back = buffer_.front();
            buffer_.pop_front();
            locker.unlock();
            cond.notify_one();
            return back;
        }
    }
    WriteQueue() {}
private:
    std::mutex mu;
    std::condition_variable cond;
    std::deque<Data> buffer_;
};

WriteQueue<Message> writeQueue;

class ReceiveThread : public AsyncProgressQueueWorker<char> {
 public:
  ReceiveThread(
      Callback *callback
    , Callback *progress)
    : AsyncProgressQueueWorker(callback), progress(progress) {}
  ~ReceiveThread() {
      delete progress;
  }

  void Execute (const AsyncProgressQueueWorker::ExecutionProgress& progress) {
    char *response;
    int result;
    int connected = 1;
    int timeout = 0;
    int pending = 0;
    size_t size = 4096;

    response = new char[4096];

    while(connected) {
        result = sctp_recvmsg(sock, (void *)response, size, NULL, 0, 0, 0);
        if (result > 0 && result < 4095) {
            if (debug) {
                struct tm *now = getCurrentTime();
                printf("%i/%i/%i %i:%i:%i ", now->tm_year + 1900, now->tm_mon + 1, now->tm_mday, now->tm_hour, now->tm_min, now->tm_sec);
                printf("Server replied (size %d)\n", result);
            }
            pending = 0;
            progress.Send((const char*)response, size_t(result));
            result = 0;
        }
        else {
            if ((result == -1 && (errno != EWOULDBLOCK || errno != EAGAIN) ) || pending) {
                if (timeout == 0) {
                    printf("Can't receive from other end. Waiting for 1 minute. Error code: %d\n", errno);
                    pending = 1;
                }
                if (timeout >= 3000) {
                    connected = 0;
                    close(sock);
                }
                else {
                    timeout += 5;
                    usleep(5000);
                }
            }
            else {
                usleep(5000);
            }
        }
    }
  }

  void HandleProgressCallback(const char *data, size_t count) {
    HandleScope scope;

    char *buf = (char *)malloc(count);

    for (int i = 0; i < (int)count; i++) {
      buf[i] = data[i];
    }

    v8::Local<v8::Value> argv[] = {
        NewBuffer(buf, count).ToLocalChecked()
    };

    progress->Call(1, argv);
  }

 private:
  Callback *progress;
};

class SendThread : public AsyncProgressQueueWorker<char> {
 public:
  SendThread(
      Callback *callback
    , Callback *errorcb
    , WriteQueue<Message> *writeQueue)
    : AsyncProgressQueueWorker(callback), errorcb(errorcb), writeQueue(writeQueue) {}
  ~SendThread() {
      delete errorcb;
      delete writeQueue;
  }

  void Execute (const AsyncProgressQueueWorker::ExecutionProgress& progress) {
    int result;
    int connected = 1;
    int sent = 0;

    while(connected) {
        Message data = writeQueue->pop();
        sent = 0;

        while(!sent) {

            result = sctp_sendmsg (sock, (void *) data.buffer, data.length, NULL, 0, 0, 0, 0, 0, 0);

            if (result == -1) {
                if (errno != EAGAIN || errno != EWOULDBLOCK) {
                    progress.Send("", result);
                    sent = 1;
                    connected = false;
                }
                else {
                    usleep(5000);
                }
            }
            else {
                progress.Send("", result);
                sent = 1;
            }
        }
    }
  }

  void HandleProgressCallback(const char *data, size_t count) {
    HandleScope scope;

    if ((int)count < 0) {
        if (debug) {
            printf("Cannot send message. Error code: %d\n", errno);
        }
        std::string error = "Cannot send message.";
        Local<Value> argv[] = {
          New<v8::String>(error.c_str()).ToLocalChecked()
        };
        errorcb->Call(1, argv);
    }
    else {

      if (debug) {
          struct tm *now = getCurrentTime();
          printf("%i/%i/%i %i:%i:%i ", now->tm_year + 1900, now->tm_mon + 1, now->tm_mday, now->tm_hour, now->tm_min, now->tm_sec);
          printf("Message successfully sent (%d bytes).\n", (int)count);
      }
    }
  }

 private:
  Callback *errorcb;
  WriteQueue<Message> *writeQueue;
};


void Debug(const Nan::FunctionCallbackInfo<v8::Value>& args) {

    debug = args[0]->NumberValue();
}

NAN_METHOD (Client) {
    Callback *onData = new Callback(info[4].As<v8::Function>());
    Callback *onDisconnect = new Callback(info[5].As<v8::Function>());
    Callback *onError = new Callback(info[6].As<v8::Function>());
    Callback *cb = new Callback(info[7].As<v8::Function>());
    int result;
    struct sockaddr_in *remoteAddrs, *addrs, addr;
    struct sctp_initmsg initmsg;
    struct timeval timeout;
    sctp_assoc_t id;
    fd_set fdset;

    errno = 0;

    if (!info[0]->IsArray()) {
        std::string error = "Hosts must be an array";
        Local<Value> argv[] = {
          New<v8::String>(error.c_str()).ToLocalChecked()
        };
        cb->Call(1, argv);
        return;
    }

    if (!info[1]->IsArray()) {
        std::string error = "Remote hosts must be an array";
        Local<Value> argv[] = {
          New<v8::String>(error.c_str()).ToLocalChecked()
        };
        cb->Call(1, argv);
        return;
    }

    if (!info[2]->IsNumber()) {
        std::string error = "Port must be a number";
        Local<Value> argv[] = {
          New<v8::String>(error.c_str()).ToLocalChecked()
        };
        cb->Call(1, argv);
        return;
    }

    if (!info[3]->IsObject()) {
        std::string error = "Init message options must be an object";
        Local<Value> argv[] = {
          New<v8::String>(error.c_str()).ToLocalChecked()
        };
        cb->Call(1, argv);
        return;
    }

    Local<Array> hosts = Local<Array>::Cast(info[0]);
    Local<Array> remoteHosts = Local<Array>::Cast(info[1]);
    int port = info[2]->NumberValue();
    Local<Object> initOpts = info[3]->ToObject();

    sock = socket (PF_INET, SOCK_STREAM|SOCK_NONBLOCK, IPPROTO_SCTP);

    if (sock == -1) {
        if (debug) {
            printf("Unable to create socket. Error code: %d\n", errno);
        }
        std::string error = "Unable to create socket.";
        Local<Value> argv[] = {
          New<v8::String>(error.c_str()).ToLocalChecked()
        };
        cb->Call(1, argv);
        return;
    }

    addrs = (struct sockaddr_in *) malloc(sizeof(struct sockaddr_in) * hosts->Array::Length());

    for (int i = 0; i < (int)hosts->Array::Length(); i++) {

        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr(*String::Utf8Value(hosts->Get(i)->ToString()));

        memcpy(addrs + i, &addr, sizeof(struct sockaddr_in));
    }

    result = sctp_bindx(sock, (struct sockaddr *)addrs, hosts->Array::Length(), SCTP_BINDX_ADD_ADDR);
    free(addrs);

    if (result == -1)
    {
        if (debug) {
            printf("Cannot bind socket to specified addresses. Error code: %d\n", errno);
        }
        close(sock);
        std::string error = "Cannot bind socket to specified addresses.";
        Local<Value> argv[] = {
          New<v8::String>(error.c_str()).ToLocalChecked()
        };
        cb->Call(1, argv);
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
        close(sock);
        std::string error = "Cannot set init options to socket.";
        Local<Value> argv[] = {
          New<v8::String>(error.c_str()).ToLocalChecked()
        };
        cb->Call(1, argv);
        return;
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
        close(sock);
        std::string error = "Cannot connect to specified addresses.";
        Local<Value> argv[] = {
          New<v8::String>(error.c_str()).ToLocalChecked()
        };
        cb->Call(1, argv);
        return;
    }
    else if (result == -1 && errno == EINPROGRESS) {
        if (debug) {
            printf("Trying to connect with specified addresses on specified port.\n");
        }
        free(remoteAddrs);

        int so_error;
        socklen_t opt_len = (socklen_t)sizeof(int);

        select(sock + 1, 0, &fdset, 0, &timeout);

        if (FD_ISSET(sock, &fdset)) {
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &opt_len) != 0) {
                if (debug) {
                    printf("Getsockopt error on connect. Error code: %d\n", errno);
                }
                close(sock);
                std::string error = "Getsockopt error on connect.";
                Local<Value> argv[] = {
                  New<v8::String>(error.c_str()).ToLocalChecked()
                };
                cb->Call(1, argv);
                return;
            }
            if (so_error) {
                if (debug) {
                    printf("Connection failed. Error code: %d\n", errno);
                }
                close(sock);
                std::string error = "Connection failed.";
                Local<Value> argv[] = {
                  New<v8::String>(error.c_str()).ToLocalChecked()
                };
                cb->Call(1, argv);
                return;
            }
            if (debug) {
                printf("Connection successful. Association ID: %d\n", id);
            }

            char idBuffer[10];

            snprintf(idBuffer, sizeof(idBuffer), "%d", id);

            Local<Value> argv[] = {
              Null(),
              New<String>(&idBuffer[0]).ToLocalChecked()
            };

            cb->Call(2, argv);

            AsyncQueueWorker(new ReceiveThread(
              onDisconnect
            , onData));

            AsyncQueueWorker(new SendThread(
              onDisconnect
            , onError
            , &writeQueue));

            return;
        } else {
            if (debug) {
                printf("Connection attempt timed out. Error code: %d\n", errno);
            }
            close(sock);
            std::string error = "Connection attempt timed out.";
            Local<Value> argv[] = {
              New<v8::String>(error.c_str()).ToLocalChecked()
            };
            cb->Call(1, argv);
            return;
        }
    }
}

NAN_METHOD (Send) {
    char *bufferData = node::Buffer::Data(info[0]);
    size_t bufferLength = node::Buffer::Length(info[0]);

    Message data((const char*)bufferData, bufferLength);

    writeQueue.write(data);
}

NAN_METHOD (Disconnect) {
    Callback *cb = new Callback(info[0].As<v8::Function>());

    close(sock);
    if (debug) {
        printf("Socket disconnected.");
    }
    std::string error = "Socket disconnected.";
    Local<Value> argv[] = {
      New<v8::String>(error.c_str()).ToLocalChecked()
    };
    cb->Call(1, argv);
}

void Init(v8::Local<v8::Object> exports) {
    exports->Set(Nan::New("client").ToLocalChecked(),
                Nan::New<v8::FunctionTemplate>(Client)->GetFunction());
    exports->Set(Nan::New("debug").ToLocalChecked(),
                Nan::New<v8::FunctionTemplate>(Debug)->GetFunction());
    exports->Set(Nan::New("send").ToLocalChecked(),
                Nan::New<v8::FunctionTemplate>(Send)->GetFunction());
    exports->Set(Nan::New("disconnect").ToLocalChecked(),
                    Nan::New<v8::FunctionTemplate>(Disconnect)->GetFunction());
}

NODE_MODULE(exports, Init)
